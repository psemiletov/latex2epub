#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Footnote {
    int number = 0;
    std::string raw_tex;
};

struct ImageAsset {
    std::string id;
    std::string src_path;
    std::string epub_rel_href;
    std::string media_type;
};

struct Chapter {
    std::string title;
    std::string body_tex;
    std::vector<Footnote> footnotes;
};

struct Part {
    std::string title;
    std::string intro_tex;
    std::vector<Chapter> chapters;
};

struct Book {
    std::string title = "Без названия";
    std::string author;
    std::string date;
    std::vector<Chapter> loose_chapters;
    std::vector<Part> parts;
    std::vector<ImageAsset> images;
};

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Не удалось открыть файл: " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Не удалось записать файл: " + p.string());
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static void copy_binary_file(const fs::path& src, const fs::path& dst) {
    fs::create_directories(dst.parent_path());
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("Не удалось скопировать картинку " + src.string() +
        " -> " + dst.string() + ": " + ec.message());
    }
}

static std::string current_utc_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    #if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
    #else
    gmtime_r(&t, &tm_utc);
    #endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::string make_book_identifier(const std::string& title, const std::string& author) {
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;

    auto feed = [&](const std::string& s) {
        for (unsigned char c : s) {
            hash ^= c;
            hash *= prime;
        }
    };

    feed(author);
    feed("\n");
    feed(title);

    std::ostringstream oss;
    oss << "urn:book:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

static std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::string detect_image_media_type(const fs::path& img_path) {
    std::string ext = to_lower_ascii(img_path.extension().string());
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".gif") return "image/gif";
    return "";
}

static std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool escaped = false;
    bool in_comment = false;

    for (char c : src) {
        if (in_comment) {
            if (c == '\n') {
                in_comment = false;
                escaped = false;
                out.push_back(c);
            }
            continue;
        }
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            out.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '%') {
            in_comment = true;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

static bool read_braced_group(const std::string& s, size_t open_brace_pos,
                              std::string& inside, size_t& after_close_pos) {
    if (open_brace_pos >= s.size() || s[open_brace_pos] != '{') {
        return false;
    }
    inside.clear();
    int depth = 0;
    bool escaped = false;
    for (size_t i = open_brace_pos; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) {
            if (depth > 0) inside.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            if (depth > 0) inside.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '{') {
            if (depth > 0) inside.push_back(c);
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                after_close_pos = i + 1;
                return true;
            }
            inside.push_back(c);
        } else {
            if (depth > 0) inside.push_back(c);
        }
    }
    return false;
                              }

                              static void skip_optional_brackets(const std::string& s, size_t& pos) {
                                  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
                                      ++pos;
                                  }
                                  if (pos < s.size() && s[pos] == '[') {
                                      int depth = 0;
                                      for (size_t i = pos; i < s.size(); ++i) {
                                          if (s[i] == '[') ++depth;
                                          else if (s[i] == ']') {
                                              --depth;
                                              if (depth == 0) {
                                                  pos = i + 1;
                                                  return;
                                              }
                                          }
                                      }
                                  }
                              }

                              static bool skip_braced_groups(const std::string& s, size_t pos, int count, size_t& end_pos) {
                                  size_t p = pos;
                                  for (int k = 0; k < count; ++k) {
                                      while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) {
                                          ++p;
                                      }
                                      if (p >= s.size() || s[p] != '{') {
                                          return false;
                                      }
                                      std::string dummy;
                                      size_t after = p;
                                      if (!read_braced_group(s, p, dummy, after)) {
                                          return false;
                                      }
                                      p = after;
                                  }
                                  end_pos = p;
                                  return true;
                              }

                              static bool match_macro(const std::string& s, size_t pos, const std::string& name) {
                                  std::string full = "\\" + name;
                                  if (s.compare(pos, full.size(), full) != 0) return false;
                                  size_t after = pos + full.size();
                                  return after >= s.size() || !std::isalpha(static_cast<unsigned char>(s[after]));
                              }

                              static std::string trim_spaces(const std::string& s) {
                                  size_t a = 0;
                                  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
                                  size_t b = s.size();
                                  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
                                  return s.substr(a, b - a);
                              }

                              static bool match_env_tag_at(const std::string& s, size_t pos,
                                                           const std::string& kw, const std::string& env_name,
                                                           size_t& after_tag_pos) {
                                  if (!match_macro(s, pos, kw)) return false;
                                  size_t p = pos + 1 + kw.size();
                                  while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
                                  if (p >= s.size() || s[p] != '{') return false;
                                  std::string inside;
                                  size_t after = p;
                                  if (!read_braced_group(s, p, inside, after)) return false;
                                  if (trim_spaces(inside) != env_name) return false;
                                  after_tag_pos = after;
                                  return true;
                                                           }

                                                           static size_t find_env_end(const std::string& s, size_t search_from, const std::string& env_name) {
                                                               for (size_t i = search_from; i < s.size(); ++i) {
                                                                   if (s[i] == '\\') {
                                                                       size_t dummy = i;
                                                                       if (match_env_tag_at(s, i, "end", env_name, dummy)) {
                                                                           return i;
                                                                       }
                                                                   }
                                                               }
                                                               return std::string::npos;
                                                           }

                                                           static std::string strip_unwanted_macros(const std::string& s) {
                                                               std::string out;
                                                               out.reserve(s.size());

                                                               for (size_t i = 0; i < s.size();) {
                                                                   if (s[i] == '\\') {
                                                                       if (match_macro(s, i, "vspace")) {
                                                                           size_t p = i + std::string("\\vspace").size();
                                                                           if (p < s.size() && s[p] == '*') ++p;
                                                                           size_t end_p = p;
                                                                           if (skip_braced_groups(s, p, 1, end_p)) {
                                                                               i = end_p;
                                                                               continue;
                                                                           }
                                                                       }

                                                                       if (match_macro(s, i, "addcontentsline")) {
                                                                           size_t p = i + std::string("\\addcontentsline").size();
                                                                           if (p < s.size() && s[p] == '*') ++p;
                                                                           size_t end_p = p;
                                                                           if (skip_braced_groups(s, p, 3, end_p)) {
                                                                               i = end_p;
                                                                               continue;
                                                                           }
                                                                       }

                                                                       auto try_skip_2arg = [&](const std::string& name) -> bool {
                                                                           if (!match_macro(s, i, name)) return false;
                                                                           size_t p = i + 1 + name.size();
                                                                           if (p < s.size() && s[p] == '*') ++p;
                                                                           size_t end_p = p;
                                                                           if (skip_braced_groups(s, p, 2, end_p)) {
                                                                               i = end_p;
                                                                               return true;
                                                                           }
                                                                           return false;
                                                                       };
                                                                       if (try_skip_2arg("markboth") || try_skip_2arg("markright")) {
                                                                           continue;
                                                                       }

                                                                       static const std::vector<std::string> zero_arg_noise = {
                                                                           "tableofcontents",
                                                                           "maketitle",
                                                                           "newpage",
                                                                           "clearpage",
                                                                           "cleardoublepage",
                                                                           "noindent",
                                                                           "centering",
                                                                           "phantomsection",
                                                                           "medskip"
                                                                       };
                                                                       bool skipped_zero = false;
                                                                       for (const auto& name : zero_arg_noise) {
                                                                           if (match_macro(s, i, name)) {
                                                                               size_t p = i + 1 + name.size();
                                                                               if (p < s.size() && s[p] == '*') ++p;
                                                                               if (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
                                                                               i = p;
                                                                               skipped_zero = true;
                                                                               break;
                                                                           }
                                                                       }
                                                                       if (skipped_zero) {
                                                                           continue;
                                                                       }
                                                                   }

                                                                   out.push_back(s[i++]);
                                                               }
                                                               return out;
                                                           }

                                                           static std::string unwrap_italic_if_whole(const std::string& raw) {
                                                               std::string t = trim_spaces(raw);
                                                               for (const char* cmd : {"\\textit", "\\emph"}) {
                                                                   size_t cmd_len = std::char_traits<char>::length(cmd);
                                                                   if (t.compare(0, cmd_len, cmd) == 0) {
                                                                       size_t p = cmd_len;
                                                                       while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p;
                                                                       std::string inner;
                                                                       size_t after = p;
                                                                       if (p < t.size() && t[p] == '{' && read_braced_group(t, p, inner, after)) {
                                                                           if (trim_spaces(t.substr(after)).empty()) {
                                                                               return trim_spaces(inner);
                                                                           }
                                                                       }
                                                                   }
                                                               }
                                                               return t;
                                                           }

                                                           // Пути к изображениям строятся от корневого каталога книги;
                                                           // при отсутствии точного совпадения проверяем регистр имени файла в каталоге.
                                                           static fs::path resolve_image_path(const fs::path& book_root_dir,
                                                                                              const fs::path& target) {
                                                               fs::path candidate = target.is_absolute() ? target : (book_root_dir / target);
                                                               std::error_code ec;
                                                               if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
                                                                   return fs::weakly_canonical(candidate, ec);
                                                               }

                                                               fs::path dir = candidate.parent_path();
                                                               std::string wanted = to_lower_ascii(candidate.filename().string());
                                                               if (!wanted.empty() && fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
                                                                   for (const auto& entry : fs::directory_iterator(dir, ec)) {
                                                                       if (entry.is_regular_file(ec) &&
                                                                           to_lower_ascii(entry.path().filename().string()) == wanted) {
                                                                           return fs::weakly_canonical(entry.path(), ec);
                                                                           }
                                                                   }
                                                               }

                                                               return candidate;
                                                                                              }

                                                                                              static std::string expand_inputs(const std::string& tex,
                                                                                                                               const fs::path& base_dir,
                                                                                                                               const fs::path& book_root_dir,
                                                                                                                               std::vector<fs::path>& stack) {
                                                                                                  std::string clean = strip_comments(tex);
                                                                                                  std::string out;
                                                                                                  out.reserve(clean.size());

                                                                                                  const std::string cmd_input = "\\input";
                                                                                                  const std::string cmd_inc = "\\includegraphics";

                                                                                                  for (size_t i = 0; i < clean.size();) {
                                                                                                      if (clean.compare(i, cmd_input.size(), cmd_input) == 0) {
                                                                                                          size_t j = i + cmd_input.size();
                                                                                                          if (j < clean.size() && std::isalpha(static_cast<unsigned char>(clean[j]))) {
                                                                                                              out.push_back(clean[i++]);
                                                                                                              continue;
                                                                                                          }
                                                                                                          while (j < clean.size() && std::isspace(static_cast<unsigned char>(clean[j]))) {
                                                                                                              ++j;
                                                                                                          }
                                                                                                          std::string target;
                                                                                                          size_t next_pos = j;
                                                                                                          if (j < clean.size() && clean[j] == '{') {
                                                                                                              if (!read_braced_group(clean, j, target, next_pos)) {
                                                                                                                  throw std::runtime_error("Незакрытая фигурная скобка в \\input");
                                                                                                              }
                                                                                                          } else {
                                                                                                              while (next_pos < clean.size() &&
                                                                                                                  !std::isspace(static_cast<unsigned char>(clean[next_pos])) &&
                                                                                                                  clean[next_pos] != '\\') {
                                                                                                                  target.push_back(clean[next_pos++]);
                                                                                                                  }
                                                                                                          }

                                                                                                          fs::path inc_path = base_dir / target;
                                                                                                          if (!inc_path.has_extension()) {
                                                                                                              inc_path += ".tex";
                                                                                                          }
                                                                                                          inc_path = fs::weakly_canonical(inc_path);

                                                                                                          if (std::find(stack.begin(), stack.end(), inc_path) != stack.end()) {
                                                                                                              throw std::runtime_error("Циклический \\input: " + inc_path.string());
                                                                                                          }
                                                                                                          stack.push_back(inc_path);
                                                                                                          std::string child_content = read_file(inc_path);
                                                                                                          out += expand_inputs(child_content, inc_path.parent_path(), book_root_dir, stack);
                                                                                                          stack.pop_back();
                                                                                                          i = next_pos;
                                                                                                          continue;
                                                                                                      }

                                                                                                      if (match_macro(clean, i, "includegraphics")) {
                                                                                                          size_t p = i + cmd_inc.size();
                                                                                                          if (p < clean.size() && clean[p] == '*') ++p;
                                                                                                          skip_optional_brackets(clean, p);
                                                                                                          while (p < clean.size() && std::isspace(static_cast<unsigned char>(clean[p]))) ++p;
                                                                                                          std::string img_target;
                                                                                                          size_t after_img = p;
                                                                                                          if (p < clean.size() && clean[p] == '{' && read_braced_group(clean, p, img_target, after_img)) {
                                                                                                              fs::path resolved = resolve_image_path(book_root_dir, fs::path(trim_spaces(img_target)));
                                                                                                              out += "\\includegraphics{" + resolved.string() + "}";
                                                                                                              i = after_img;
                                                                                                              continue;
                                                                                                          }
                                                                                                      }

                                                                                                      out.push_back(clean[i++]);
                                                                                                  }
                                                                                                  return out;
                                                                                                                               }

                                                                                                                               static std::string xml_escape(const std::string& s) {
                                                                                                                                   std::string r;
                                                                                                                                   r.reserve(s.size());
                                                                                                                                   for (char c : s) {
                                                                                                                                       switch (c) {
                                                                                                                                           case '&':  r += "&amp;"; break;
                                                                                                                                           case '<':  r += "&lt;"; break;
                                                                                                                                           case '>':  r += "&gt;"; break;
                                                                                                                                           case '"':  r += "&quot;"; break;
                                                                                                                                           default:   r.push_back(c); break;
                                                                                                                                       }
                                                                                                                                   }
                                                                                                                                   return r;
                                                                                                                               }

                                                                                                                               static std::string register_image(Book& book, const std::string& raw_img_path) {
                                                                                                                                   fs::path src_p(trim_spaces(raw_img_path));
                                                                                                                                   std::error_code ec;
                                                                                                                                   if (!fs::exists(src_p, ec) || !fs::is_regular_file(src_p, ec)) {
                                                                                                                                       throw std::runtime_error("Файл иллюстрации не найден: " + src_p.string());
                                                                                                                                   }
                                                                                                                                   std::string media = detect_image_media_type(src_p);
                                                                                                                                   if (media.empty()) {
                                                                                                                                       throw std::runtime_error("Неподдерживаемый формат иллюстрации для EPUB: " + src_p.string());
                                                                                                                                   }

                                                                                                                                   fs::path canon_src = fs::weakly_canonical(src_p, ec);
                                                                                                                                   std::string canon_str = ec ? src_p.string() : canon_src.string();
                                                                                                                                   for (const auto& existing : book.images) {
                                                                                                                                       if (existing.src_path == canon_str) {
                                                                                                                                           return existing.epub_rel_href;
                                                                                                                                       }
                                                                                                                                   }

                                                                                                                                   int idx = static_cast<int>(book.images.size()) + 1;
                                                                                                                                   std::string fname = src_p.filename().string();
                                                                                                                                   std::string rel_href = "images/" + std::to_string(idx) + "_" + fname;
                                                                                                                                   book.images.push_back(ImageAsset{
                                                                                                                                       "img-" + std::to_string(idx),
                                                                                                                                                         canon_str,
                                                                                                                                                         rel_href,
                                                                                                                                                         media
                                                                                                                                   });
                                                                                                                                   return rel_href;
                                                                                                                               }

                                                                                                                               static std::string latex_inline_to_xhtml(const std::string& s,
                                                                                                                                                                        const std::string& chapter_id,
                                                                                                                                                                        std::vector<Footnote>* footnotes_out,
                                                                                                                                                                        Book* book_out = nullptr) {
                                                                                                                                   std::string out;
                                                                                                                                   out.reserve(s.size() + 64);

                                                                                                                                   for (size_t i = 0; i < s.size();) {
                                                                                                                                       if (s.compare(i, 3, "---") == 0) {
                                                                                                                                           out += "\xE2\x80\x94";
                                                                                                                                           i += 3;
                                                                                                                                           continue;
                                                                                                                                       }
                                                                                                                                       if (s.compare(i, 2, "--") == 0) {
                                                                                                                                           out += "\xE2\x80\x93";
                                                                                                                                           i += 2;
                                                                                                                                           continue;
                                                                                                                                       }
                                                                                                                                       if (s.compare(i, 2, "``") == 0) {
                                                                                                                                           out += "\xC2\xAB";
                                                                                                                                           i += 2;
                                                                                                                                           continue;
                                                                                                                                       }
                                                                                                                                       if (s.compare(i, 2, "''") == 0) {
                                                                                                                                           out += "\xC2\xBB";
                                                                                                                                           i += 2;
                                                                                                                                           continue;
                                                                                                                                       }
                                                                                                                                       if (s[i] == '~') {
                                                                                                                                           out += "\xC2\xA0";
                                                                                                                                           ++i;
                                                                                                                                           continue;
                                                                                                                                       }

                                                                                                                                       if (s[i] == '\\') {
                                                                                                                                           if (i + 1 < s.size()) {
                                                                                                                                               char nxt = s[i + 1];
                                                                                                                                               if (nxt == '&' || nxt == '%' || nxt == '$' || nxt == '#' || nxt == '_' || nxt == '{' || nxt == '}') {
                                                                                                                                                   out.push_back(nxt);
                                                                                                                                                   i += 2;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                               if (nxt == '\\') {
                                                                                                                                                   out += "<br />";
                                                                                                                                                   i += 2;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                           }

                                                                                                                                           size_t name_start = i + 1;
                                                                                                                                           size_t name_end = name_start;
                                                                                                                                           while (name_end < s.size() && std::isalpha(static_cast<unsigned char>(s[name_end]))) {
                                                                                                                                               ++name_end;
                                                                                                                                           }
                                                                                                                                           std::string cmd = s.substr(name_start, name_end - name_start);

                                                                                                                                           size_t p = name_end;
                                                                                                                                           if (cmd == "includegraphics") {
                                                                                                                                               if (p < s.size() && s[p] == '*') ++p;
                                                                                                                                               skip_optional_brackets(s, p);
                                                                                                                                           }
                                                                                                                                           while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) {
                                                                                                                                               ++p;
                                                                                                                                           }

                                                                                                                                           if (cmd == "includegraphics" && book_out != nullptr && p < s.size() && s[p] == '{') {
                                                                                                                                               std::string img_raw;
                                                                                                                                               size_t after = p;
                                                                                                                                               if (read_braced_group(s, p, img_raw, after)) {
                                                                                                                                                   std::string rel_href = register_image(*book_out, img_raw);
                                                                                                                                                   out += "<figure class=\"illustration\"><img src=\"" + xml_escape(rel_href) +
                                                                                                                                                   "\" alt=\"\" /></figure>";
                                                                                                                                                   i = after;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                           }

                                                                                                                                           if (cmd == "href" && p < s.size() && s[p] == '{') {
                                                                                                                                               std::string url_raw;
                                                                                                                                               size_t after_url = p;
                                                                                                                                               if (read_braced_group(s, p, url_raw, after_url)) {
                                                                                                                                                   size_t q = after_url;
                                                                                                                                                   while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) {
                                                                                                                                                       ++q;
                                                                                                                                                   }
                                                                                                                                                   std::string label_raw;
                                                                                                                                                   size_t after_label = q;
                                                                                                                                                   if (q < s.size() && s[q] == '{' && read_braced_group(s, q, label_raw, after_label)) {
                                                                                                                                                       std::string label_html = latex_inline_to_xhtml(label_raw, chapter_id, footnotes_out, book_out);
                                                                                                                                                       out += "<a href=\"" + xml_escape(url_raw) + "\">" + label_html + "</a>";
                                                                                                                                                       i = after_label;
                                                                                                                                                       continue;
                                                                                                                                                   }
                                                                                                                                               }
                                                                                                                                           }

                                                                                                                                           if (cmd == "footnote" && footnotes_out != nullptr && p < s.size() && s[p] == '{') {
                                                                                                                                               std::string fn_raw;
                                                                                                                                               size_t after = p;
                                                                                                                                               if (read_braced_group(s, p, fn_raw, after)) {
                                                                                                                                                   int num = static_cast<int>(footnotes_out->size()) + 1;
                                                                                                                                                   footnotes_out->push_back(Footnote{num, fn_raw});
                                                                                                                                                   std::string ref_id = "fnref-" + chapter_id + "-" + std::to_string(num);
                                                                                                                                                   std::string fn_id  = "fn-"    + chapter_id + "-" + std::to_string(num);
                                                                                                                                                   out += "<a id=\"" + ref_id + "\" href=\"#" + fn_id +
                                                                                                                                                   "\" epub:type=\"noteref\">[" + std::to_string(num) + "]</a>";
                                                                                                                                                   i = after;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                           }

                                                                                                                                           if ((cmd == "emph" || cmd == "textit") && p < s.size() && s[p] == '{') {
                                                                                                                                               std::string inner;
                                                                                                                                               size_t after = p;
                                                                                                                                               if (read_braced_group(s, p, inner, after)) {
                                                                                                                                                   out += "<em>" + latex_inline_to_xhtml(inner, chapter_id, footnotes_out, book_out) + "</em>";
                                                                                                                                                   i = after;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                           }

                                                                                                                                           if (cmd == "textbf" && p < s.size() && s[p] == '{') {
                                                                                                                                               std::string inner;
                                                                                                                                               size_t after = p;
                                                                                                                                               if (read_braced_group(s, p, inner, after)) {
                                                                                                                                                   out += "<strong>" + latex_inline_to_xhtml(inner, chapter_id, footnotes_out, book_out) + "</strong>";
                                                                                                                                                   i = after;
                                                                                                                                                   continue;
                                                                                                                                               }
                                                                                                                                           }
                                                                                                                                       }

                                                                                                                                       switch (s[i]) {
                                                                                                                                           case '&':  out += "&amp;"; break;
                                                                                                                                           case '<':  out += "&lt;"; break;
                                                                                                                                           case '>':  out += "&gt;"; break;
                                                                                                                                           case '"':  out += "&quot;"; break;
                                                                                                                                           default:   out.push_back(s[i]); break;
                                                                                                                                       }
                                                                                                                                       ++i;
                                                                                                                                   }
                                                                                                                                   return out;
                                                                                                                                                                        }

                                                                                                                                                                        static bool try_render_center_illustration(const std::string& inner_tex,
                                                                                                                                                                                                                   const std::string& chapter_id,
                                                                                                                                                                                                                   std::vector<Footnote>& footnotes,
                                                                                                                                                                                                                   Book& book,
                                                                                                                                                                                                                   std::string& out_xhtml) {
                                                                                                                                                                            size_t pos = 0;
                                                                                                                                                                            while (pos < inner_tex.size() && std::isspace(static_cast<unsigned char>(inner_tex[pos]))) {
                                                                                                                                                                                ++pos;
                                                                                                                                                                            }
                                                                                                                                                                            if (!match_macro(inner_tex, pos, "includegraphics")) {
                                                                                                                                                                                return false;
                                                                                                                                                                            }

                                                                                                                                                                            size_t p = pos + std::string("\\includegraphics").size();
                                                                                                                                                                            if (p < inner_tex.size() && inner_tex[p] == '*') ++p;
                                                                                                                                                                            skip_optional_brackets(inner_tex, p);
                                                                                                                                                                            while (p < inner_tex.size() && std::isspace(static_cast<unsigned char>(inner_tex[p]))) {
                                                                                                                                                                                ++p;
                                                                                                                                                                            }

                                                                                                                                                                            std::string img_raw;
                                                                                                                                                                            size_t after_img = p;
                                                                                                                                                                            if (p >= inner_tex.size() || inner_tex[p] != '{' ||
                                                                                                                                                                                !read_braced_group(inner_tex, p, img_raw, after_img)) {
                                                                                                                                                                                return false;
                                                                                                                                                                                }

                                                                                                                                                                                std::string rel_href = register_image(book, img_raw);
                                                                                                                                                                                std::string rest_raw = unwrap_italic_if_whole(inner_tex.substr(after_img));

                                                                                                                                                                                std::ostringstream fig;
                                                                                                                                                                                fig << "  <figure class=\"illustration\">\n";
                                                                                                                                                                                if (rest_raw.empty()) {
                                                                                                                                                                                    fig << "    <img src=\"" << xml_escape(rel_href) << "\" alt=\"\" />\n";
                                                                                                                                                                                } else {
                                                                                                                                                                                    std::string cap_html = latex_inline_to_xhtml(rest_raw, chapter_id, &footnotes, &book);
                                                                                                                                                                                    std::string plain_alt = latex_inline_to_xhtml(rest_raw, chapter_id, nullptr, nullptr);
                                                                                                                                                                                    fig << "    <img src=\"" << xml_escape(rel_href) << "\" alt=\"" << xml_escape(plain_alt) << "\" />\n";
                                                                                                                                                                                    fig << "    <figcaption>" << cap_html << "</figcaption>\n";
                                                                                                                                                                                }
                                                                                                                                                                                fig << "  </figure>\n";

                                                                                                                                                                                out_xhtml = fig.str();
                                                                                                                                                                                return true;
                                                                                                                                                                                                                   }

                                                                                                                                                                                                                   static std::string render_blocks_with_footnotes(const std::string& raw_tex,
                                                                                                                                                                                                                                                                const std::string& chapter_id,
                                                                                                                                                                                                                                                                std::vector<Footnote>& footnotes,
                                                                                                                                                                                                                                                                Book& book,
                                                                                                                                                                                                                                                                bool emit_footnotes_section = true) {
                                                                                                                                                                                                                       std::string norm;
                                                                                                                                                                                                                       norm.reserve(raw_tex.size());
                                                                                                                                                                                                                       for (size_t i = 0; i < raw_tex.size(); ++i) {
                                                                                                                                                                                                                           if (raw_tex[i] == '\r' && i + 1 < raw_tex.size() && raw_tex[i + 1] == '\n') {
                                                                                                                                                                                                                               continue;
                                                                                                                                                                                                                           }
                                                                                                                                                                                                                           norm.push_back(raw_tex[i]);
                                                                                                                                                                                                                       }

                                                                                                                                                                                                                       std::ostringstream body;
                                                                                                                                                                                                                       std::string current_para;

                                                                                                                                                                                                                       auto flush_para = [&]() {
                                                                                                                                                                                                                           if (!current_para.empty()) {
                                                                                                                                                                                                                               std::string html_para = latex_inline_to_xhtml(current_para, chapter_id, &footnotes, &book);
                                                                                                                                                                                                                               if (!trim_spaces(html_para).empty()) {
                                                                                                                                                                                                                                   body << "  <p>" << html_para << "</p>\n";
                                                                                                                                                                                                                               }
                                                                                                                                                                                                                               current_para.clear();
                                                                                                                                                                                                                           }
                                                                                                                                                                                                                       };

                                                                                                                                                                                                                       for (size_t i = 0; i < norm.size();) {
                                                                                                                                                                                                                           size_t after_begin = i;
                                                                                                                                                                                                                           if (match_env_tag_at(norm, i, "begin", "center", after_begin)) {
                                                                                                                                                                                                                               size_t end_pos = find_env_end(norm, after_begin, "center");
                                                                                                                                                                                                                               if (end_pos != std::string::npos) {
                                                                                                                                                                                                                                   std::string inside = norm.substr(after_begin, end_pos - after_begin);
                                                                                                                                                                                                                                   std::string fig_html;
                                                                                                                                                                                                                                   if (try_render_center_illustration(inside, chapter_id, footnotes, book, fig_html)) {
                                                                                                                                                                                                                                       flush_para();
                                                                                                                                                                                                                                       body << fig_html;
                                                                                                                                                                                                                                       size_t after_end = end_pos;
                                                                                                                                                                                                                                       match_env_tag_at(norm, end_pos, "end", "center", after_end);
                                                                                                                                                                                                                                       i = after_end;
                                                                                                                                                                                                                                       continue;
                                                                                                                                                                                                                                   }
                                                                                                                                                                                                                               }
                                                                                                                                                                                                                           }

                                                                                                                                                                                                                           if (match_env_tag_at(norm, i, "begin", "quotation", after_begin)) {
                                                                                                                                                                                                                               size_t end_pos = find_env_end(norm, after_begin, "quotation");
                                                                                                                                                                                                                               if (end_pos != std::string::npos) {
                                                                                                                                                                                                                                   flush_para();
                                                                                                                                                                                                                                   std::string inside = norm.substr(after_begin, end_pos - after_begin);
                                                                                                                                                                                                                                   std::string inner_html = render_blocks_with_footnotes(inside, chapter_id, footnotes, book, false);
                                                                                                                                                                                                                                   body << "  <blockquote class=\"quotation\">\n"
                                                                                                                                                                                                                                   << inner_html
                                                                                                                                                                                                                                   << "  </blockquote>\n";
                                                                                                                                                                                                                                   size_t after_end = end_pos;
                                                                                                                                                                                                                                   match_env_tag_at(norm, end_pos, "end", "quotation", after_end);
                                                                                                                                                                                                                                   i = after_end;
                                                                                                                                                                                                                                   continue;
                                                                                                                                                                                                                               }
                                                                                                                                                                                                                           }

                                                                                                                                                                                                                           if (norm[i] == '\n' && i + 1 < norm.size() && norm[i + 1] == '\n') {
                                                                                                                                                                                                                               flush_para();
                                                                                                                                                                                                                               while (i < norm.size() && norm[i] == '\n') {
                                                                                                                                                                                                                                   ++i;
                                                                                                                                                                                                                               }
                                                                                                                                                                                                                               continue;
                                                                                                                                                                                                                           }

                                                                                                                                                                                                                           if (norm[i] == '\n') {
                                                                                                                                                                                                                               if (!current_para.empty() && current_para.back() != ' ') {
                                                                                                                                                                                                                                   current_para.push_back(' ');
                                                                                                                                                                                                                               }
                                                                                                                                                                                                                               ++i;
                                                                                                                                                                                                                               continue;
                                                                                                                                                                                                                           }

                                                                                                                                                                                                                           current_para.push_back(norm[i++]);
                                                                                                                                                                                                                       }

                                                                                                                                                                                                                       flush_para();

                                                                                                                                                                                                                       if (emit_footnotes_section && !footnotes.empty()) {
                                                                                                                                                                                                                           body << "\n  <section epub:type=\"footnotes\" class=\"footnotes\">\n";
                                                                                                                                                                                                                           body << "    <hr />\n";
                                                                                                                                                                                                                           body << "    <ol>\n";
                                                                                                                                                                                                                           for (const auto& fn : footnotes) {
                                                                                                                                                                                                                               std::string ref_id = "fnref-" + chapter_id + "-" + std::to_string(fn.number);
                                                                                                                                                                                                                               std::string fn_id  = "fn-"    + chapter_id + "-" + std::to_string(fn.number);
                                                                                                                                                                                                                               std::string fn_html = latex_inline_to_xhtml(fn.raw_tex, chapter_id, nullptr, &book);
                                                                                                                                                                                                                               body << "      <li id=\"" << fn_id << "\" epub:type=\"footnote\">"
                                                                                                                                                                                                                               << fn_html
                                                                                                                                                                                                                               << " <a href=\"#" << ref_id << "\" epub:type=\"backlink\">&#8617;</a></li>\n";
                                                                                                                                                                                                                           }
                                                                                                                                                                                                                           body << "    </ol>\n";
                                                                                                                                                                                                                           body << "  </section>\n";
                                                                                                                                                                                                                       }

                                                                                                                                                                                                                       return body.str();
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static Book parse_book(const std::string& flat_tex) {
                                                                                                                                                                                                                                                                Book book;
                                                                                                                                                                                                                                                                bool has_part = false;
                                                                                                                                                                                                                                                                bool in_chapter = false;
                                                                                                                                                                                                                                                                Chapter current_chapter;
                                                                                                                                                                                                                                                                Part current_part;

                                                                                                                                                                                                                                                                auto flush_chapter = [&]() {
                                                                                                                                                                                                                                                                if (!in_chapter) return;
                                                                                                                                                                                                                                                                if (has_part) {
                                                                                                                                                                                                                                                                current_part.chapters.push_back(current_chapter);
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                book.loose_chapters.push_back(current_chapter);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                current_chapter = Chapter{};
                                                                                                                                                                                                                                                                in_chapter = false;
                                                                                                                                                                                                                                                                };

                                                                                                                                                                                                                                                                auto flush_part = [&]() {
                                                                                                                                                                                                                                                                flush_chapter();
                                                                                                                                                                                                                                                                if (has_part) {
                                                                                                                                                                                                                                                                book.parts.push_back(current_part);
                                                                                                                                                                                                                                                                current_part = Part{};
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                };

                                                                                                                                                                                                                                                                auto append_text = [&](char c) {
                                                                                                                                                                                                                                                                if (in_chapter) {
                                                                                                                                                                                                                                                                current_chapter.body_tex.push_back(c);
                                                                                                                                                                                                                                                                } else if (has_part) {
                                                                                                                                                                                                                                                                current_part.intro_tex.push_back(c);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                };

                                                                                                                                                                                                                                                                for (size_t i = 0; i < flat_tex.size();) {
                                                                                                                                                                                                                                                                if (flat_tex[i] == '\\') {
                                                                                                                                                                                                                                                                auto try_read_arg = [&](const std::string& macro_name, std::string& arg, size_t& next_i) -> bool {
                                                                                                                                                                                                                                                                if (!match_macro(flat_tex, i, macro_name)) return false;
                                                                                                                                                                                                                                                                size_t p = i + 1 + macro_name.size();
                                                                                                                                                                                                                                                                if (p < flat_tex.size() && flat_tex[p] == '*') ++p;
                                                                                                                                                                                                                                                                while (p < flat_tex.size() && std::isspace(static_cast<unsigned char>(flat_tex[p]))) ++p;
                                                                                                                                                                                                                                                                if (p < flat_tex.size() && flat_tex[p] == '{') {
                                                                                                                                                                                                                                                                if (read_braced_group(flat_tex, p, arg, next_i)) {
                                                                                                                                                                                                                                                                return true;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                return false;
                                                                                                                                                                                                                                                                };

                                                                                                                                                                                                                                                                std::string arg;
                                                                                                                                                                                                                                                                size_t next_i = i;

                                                                                                                                                                                                                                                                if (try_read_arg("title", arg, next_i)) {
                                                                                                                                                                                                                                                                book.title = arg;
                                                                                                                                                                                                                                                                i = next_i;
                                                                                                                                                                                                                                                                continue;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (try_read_arg("author", arg, next_i)) {
                                                                                                                                                                                                                                                                book.author = arg;
                                                                                                                                                                                                                                                                i = next_i;
                                                                                                                                                                                                                                                                continue;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (try_read_arg("date", arg, next_i)) {
                                                                                                                                                                                                                                                                book.date = arg;
                                                                                                                                                                                                                                                                i = next_i;
                                                                                                                                                                                                                                                                continue;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (try_read_arg("part", arg, next_i)) {
                                                                                                                                                                                                                                                                flush_part();
                                                                                                                                                                                                                                                                has_part = true;
                                                                                                                                                                                                                                                                current_part.title = arg;
                                                                                                                                                                                                                                                                i = next_i;
                                                                                                                                                                                                                                                                continue;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (try_read_arg("chapter", arg, next_i)) {
                                                                                                                                                                                                                                                                flush_chapter();
                                                                                                                                                                                                                                                                in_chapter = true;
                                                                                                                                                                                                                                                                current_chapter.title = arg;
                                                                                                                                                                                                                                                                i = next_i;
                                                                                                                                                                                                                                                                continue;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                append_text(flat_tex[i++]);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                flush_part();
                                                                                                                                                                                                                                                                return book;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static std::string xhtml_header(const std::string& doc_title) {
                                                                                                                                                                                                                                                                return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                                                                                                                                                                                                                                                "<!DOCTYPE html>\n"
                                                                                                                                                                                                                                                                "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"ru\" lang=\"ru\">\n"
                                                                                                                                                                                                                                                                "<head>\n"
                                                                                                                                                                                                                                                                "  <meta charset=\"utf-8\" />\n"
                                                                                                                                                                                                                                                                "  <title>" + xml_escape(doc_title) + "</title>\n"
                                                                                                                                                                                                                                                                "  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />\n"
                                                                                                                                                                                                                                                                "</head>\n"
                                                                                                                                                                                                                                                                "<body>\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static std::string xhtml_footer() {
                                                                                                                                                                                                                                                                return "</body>\n</html>\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static void emit_epub_dir(Book& book, const fs::path& out_dir) {
                                                                                                                                                                                                                                                                fs::create_directories(out_dir / "META-INF");
                                                                                                                                                                                                                                                                fs::create_directories(out_dir / "OEBPS");

                                                                                                                                                                                                                                                                write_file(out_dir / "mimetype", "application/epub+zip");

                                                                                                                                                                                                                                                                write_file(out_dir / "META-INF/container.xml",
                                                                                                                                                                                                                                                                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                                                                                                                                                                                                                                                "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                                                                                                                                                                                                                                                                "  <rootfiles>\n"
                                                                                                                                                                                                                                                                "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\" />\n"
                                                                                                                                                                                                                                                                "  </rootfiles>\n"
                                                                                                                                                                                                                                                                "</container>\n");

                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/style.css",
                                                                                                                                                                                                                                                                "body { line-height: 1.5; margin: 1em; }\n"
                                                                                                                                                                                                                                                                "h1, h2 { text-align: left; margin-top: 1.2em; margin-bottom: 0.6em; }\n"
                                                                                                                                                                                                                                                                "p { text-indent: 1.2em; margin-top: 0; margin-bottom: 0.6em; }\n"
                                                                                                                                                                                                                                                                ".quotation { margin: 1em 4em; font-size: 1em; }\n"
                                                                                                                                                                                                                                                                ".title-page { text-align: center; margin-top: 20%; }\n"
                                                                                                                                                                                                                                                                ".toc-page ol { list-style-type: none; padding-left: 1.2em; }\n"
                                                                                                                                                                                                                                                                ".toc-page > ol { padding-left: 0; }\n"
                                                                                                                                                                                                                                                                ".toc-page li { margin-bottom: 0.4em; }\n"
                                                                                                                                                                                                                                                                ".illustration { display: block; clear: both; width: 100%; box-sizing: border-box; margin: 1.4em auto; padding: 0 0.6em; text-align: center; }\n"
                                                                                                                                                                                                                                                                ".illustration img { display: block; clear: both; max-width: 92%; width: auto; height: auto; margin: 0 auto; }\n"
                                                                                                                                                                                                                                                                ".illustration figcaption { display: block; clear: both; width: 100%; text-indent: 0; margin-top: 0.5em; text-align: center; font-size: 0.9em; font-style: italic; }\n"
                                                                                                                                                                                                                                                                ".footnotes { margin-top: 2em; font-size: 0.9em; }\n");

                                                                                                                                                                                                                                                                std::ostringstream title_html;
                                                                                                                                                                                                                                                                title_html << xhtml_header(book.title);
                                                                                                                                                                                                                                                                title_html << "  <section class=\"title-page\">\n";
                                                                                                                                                                                                                                                                title_html << "    <h1>" << xml_escape(book.title) << "</h1>\n";
                                                                                                                                                                                                                                                                if (!book.author.empty()) {
                                                                                                                                                                                                                                                                title_html << "    <p class=\"author\">" << xml_escape(book.author) << "</p>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (!book.date.empty()) {
                                                                                                                                                                                                                                                                title_html << "    <p class=\"date\">" << xml_escape(book.date) << "</p>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                title_html << "  </section>\n";
                                                                                                                                                                                                                                                                title_html << xhtml_footer();
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/title.xhtml", title_html.str());

                                                                                                                                                                                                                                                                struct SpineItem {
                                                                                                                                                                                                                                                                std::string id;
                                                                                                                                                                                                                                                                std::string href;
                                                                                                                                                                                                                                                                std::string label;
                                                                                                                                                                                                                                                                int level;
                                                                                                                                                                                                                                                                };
                                                                                                                                                                                                                                                                std::vector<SpineItem> items;
                                                                                                                                                                                                                                                                items.push_back({"title", "title.xhtml", "Титульная страница", 2});

                                                                                                                                                                                                                                                                int ch_counter = 0;
                                                                                                                                                                                                                                                                int pt_counter = 0;

                                                                                                                                                                                                                                                                auto emit_chapter_file = [&](const Chapter& ch) {
                                                                                                                                                                                                                                                                ++ch_counter;
                                                                                                                                                                                                                                                                std::string id = "ch-" + std::to_string(ch_counter);
                                                                                                                                                                                                                                                                std::string fname = "chapter-" + std::to_string(ch_counter) + ".xhtml";

                                                                                                                                                                                                                                                                std::vector<Footnote> fns;
                                                                                                                                                                                                                                                                std::string body = render_blocks_with_footnotes(ch.body_tex, id, fns, book);

                                                                                                                                                                                                                                                                std::ostringstream xhtml;
                                                                                                                                                                                                                                                                xhtml << xhtml_header(ch.title);
                                                                                                                                                                                                                                                                xhtml << "  <h2>" << xml_escape(ch.title) << "</h2>\n";
                                                                                                                                                                                                                                                                xhtml << body;
                                                                                                                                                                                                                                                                xhtml << xhtml_footer();
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS" / fname, xhtml.str());

                                                                                                                                                                                                                                                                items.push_back({id, fname, ch.title, 2});
                                                                                                                                                                                                                                                                };

                                                                                                                                                                                                                                                                for (const auto& ch : book.loose_chapters) {
                                                                                                                                                                                                                                                                emit_chapter_file(ch);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                for (const auto& pt : book.parts) {
                                                                                                                                                                                                                                                                ++pt_counter;
                                                                                                                                                                                                                                                                std::string pt_id = "pt-" + std::to_string(pt_counter);
                                                                                                                                                                                                                                                                std::string pt_fname = "part-" + std::to_string(pt_counter) + ".xhtml";

                                                                                                                                                                                                                                                                std::vector<Footnote> intro_fns;
                                                                                                                                                                                                                                                                std::string intro_html = render_blocks_with_footnotes(pt.intro_tex, pt_id, intro_fns, book);

                                                                                                                                                                                                                                                                std::ostringstream pxhtml;
                                                                                                                                                                                                                                                                pxhtml << xhtml_header(pt.title);
                                                                                                                                                                                                                                                                pxhtml << "  <h1>" << xml_escape(pt.title) << "</h1>\n";
                                                                                                                                                                                                                                                                pxhtml << intro_html;
                                                                                                                                                                                                                                                                pxhtml << xhtml_footer();
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS" / pt_fname, pxhtml.str());

                                                                                                                                                                                                                                                                items.push_back({pt_id, pt_fname, pt.title, 1});

                                                                                                                                                                                                                                                                for (const auto& ch : pt.chapters) {
                                                                                                                                                                                                                                                                emit_chapter_file(ch);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                for (const auto& img : book.images) {
                                                                                                                                                                                                                                                                copy_binary_file(img.src_path, out_dir / "OEBPS" / img.epub_rel_href);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                std::ostringstream toc_page;
                                                                                                                                                                                                                                                                toc_page << xhtml_header("Оглавление");
                                                                                                                                                                                                                                                                toc_page << "  <nav class=\"toc-page\">\n"
                                                                                                                                                                                                                                                                << "    <h1>Оглавление</h1>\n"
                                                                                                                                                                                                                                                                << "    <ol>\n";

                                                                                                                                                                                                                                                                bool toc_open_part = false;
                                                                                                                                                                                                                                                                for (size_t idx = 1; idx < items.size(); ++idx) {
                                                                                                                                                                                                                                                                const auto& it = items[idx];
                                                                                                                                                                                                                                                                if (it.level == 1) {
                                                                                                                                                                                                                                                                if (toc_open_part) {
                                                                                                                                                                                                                                                                toc_page << "      </ol>\n    </li>\n";
                                                                                                                                                                                                                                                                toc_open_part = false;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                toc_page << "    <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a>\n"
                                                                                                                                                                                                                                                                << "      <ol>\n";
                                                                                                                                                                                                                                                                toc_open_part = true;
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                if (toc_open_part) {
                                                                                                                                                                                                                                                                toc_page << "        <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a></li>\n";
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                toc_page << "    <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a></li>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (toc_open_part) {
                                                                                                                                                                                                                                                                toc_page << "      </ol>\n    </li>\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                toc_page << "    </ol>\n"
                                                                                                                                                                                                                                                                << "  </nav>\n";
                                                                                                                                                                                                                                                                toc_page << xhtml_footer();
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/toc.xhtml", toc_page.str());

                                                                                                                                                                                                                                                                std::ostringstream nav;
                                                                                                                                                                                                                                                                nav << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                                                                                                                                                                                                                                                << "<!DOCTYPE html>\n"
                                                                                                                                                                                                                                                                << "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"ru\" lang=\"ru\">\n"
                                                                                                                                                                                                                                                                << "<head><meta charset=\"utf-8\" /><title>Оглавление</title></head>\n"
                                                                                                                                                                                                                                                                << "<body>\n"
                                                                                                                                                                                                                                                                << "  <nav epub:type=\"toc\" id=\"toc\">\n"
                                                                                                                                                                                                                                                                << "    <h1>Оглавление</h1>\n"
                                                                                                                                                                                                                                                                << "    <ol>\n";

                                                                                                                                                                                                                                                                bool open_part_li = false;
                                                                                                                                                                                                                                                                for (size_t idx = 1; idx < items.size(); ++idx) {
                                                                                                                                                                                                                                                                const auto& it = items[idx];
                                                                                                                                                                                                                                                                if (it.level == 1) {
                                                                                                                                                                                                                                                                if (open_part_li) {
                                                                                                                                                                                                                                                                nav << "      </ol>\n    </li>\n";
                                                                                                                                                                                                                                                                open_part_li = false;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                nav << "    <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a>\n"
                                                                                                                                                                                                                                                                << "      <ol>\n";
                                                                                                                                                                                                                                                                open_part_li = true;
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                if (open_part_li) {
                                                                                                                                                                                                                                                                nav << "        <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a></li>\n";
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                nav << "    <li><a href=\"" << xml_escape(it.href) << "\">" << xml_escape(it.label) << "</a></li>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (open_part_li) {
                                                                                                                                                                                                                                                                nav << "      </ol>\n    </li>\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                nav << "    </ol>\n"
                                                                                                                                                                                                                                                                << "  </nav>\n"
                                                                                                                                                                                                                                                                << "</body>\n</html>\n";
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/nav.xhtml", nav.str());

                                                                                                                                                                                                                                                                std::string book_id = make_book_identifier(book.title, book.author);
                                                                                                                                                                                                                                                                std::string modified_utc = current_utc_iso8601();

                                                                                                                                                                                                                                                                int max_depth = 1;
                                                                                                                                                                                                                                                                for (size_t idx = 1; idx < items.size(); ++idx) {
                                                                                                                                                                                                                                                                if (items[idx].level == 1) {
                                                                                                                                                                                                                                                                max_depth = 2;
                                                                                                                                                                                                                                                                break;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                std::ostringstream ncx;
                                                                                                                                                                                                                                                                ncx << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                                                                                                                                                                                                                                                << "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
                                                                                                                                                                                                                                                                << "  <head>\n"
                                                                                                                                                                                                                                                                << "    <meta name=\"dtb:uid\" content=\"" << xml_escape(book_id) << "\" />\n"
                                                                                                                                                                                                                                                                << "    <meta name=\"dtb:depth\" content=\"" << max_depth << "\" />\n"
                                                                                                                                                                                                                                                                << "    <meta name=\"dtb:totalPageCount\" content=\"0\" />\n"
                                                                                                                                                                                                                                                                << "    <meta name=\"dtb:maxPageNumber\" content=\"0\" />\n"
                                                                                                                                                                                                                                                                << "  </head>\n"
                                                                                                                                                                                                                                                                << "  <docTitle>\n"
                                                                                                                                                                                                                                                                << "    <text>" << xml_escape(book.title) << "</text>\n"
                                                                                                                                                                                                                                                                << "  </docTitle>\n";
                                                                                                                                                                                                                                                                if (!book.author.empty()) {
                                                                                                                                                                                                                                                                ncx << "  <docAuthor>\n"
                                                                                                                                                                                                                                                                << "    <text>" << xml_escape(book.author) << "</text>\n"
                                                                                                                                                                                                                                                                << "  </docAuthor>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                ncx << "  <navMap>\n";

                                                                                                                                                                                                                                                                int play_order = 0;
                                                                                                                                                                                                                                                                int np_counter = 0;
                                                                                                                                                                                                                                                                bool open_part_np = false;

                                                                                                                                                                                                                                                                for (size_t idx = 1; idx < items.size(); ++idx) {
                                                                                                                                                                                                                                                                const auto& it = items[idx];
                                                                                                                                                                                                                                                                if (it.level == 1) {
                                                                                                                                                                                                                                                                if (open_part_np) {
                                                                                                                                                                                                                                                                ncx << "    </navPoint>\n";
                                                                                                                                                                                                                                                                open_part_np = false;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                ++play_order;
                                                                                                                                                                                                                                                                ++np_counter;
                                                                                                                                                                                                                                                                ncx << "    <navPoint id=\"navPoint-" << np_counter << "\" playOrder=\"" << play_order << "\">\n"
                                                                                                                                                                                                                                                                << "      <navLabel><text>" << xml_escape(it.label) << "</text></navLabel>\n"
                                                                                                                                                                                                                                                                << "      <content src=\"" << xml_escape(it.href) << "\" />\n";
                                                                                                                                                                                                                                                                open_part_np = true;
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                ++play_order;
                                                                                                                                                                                                                                                                ++np_counter;
                                                                                                                                                                                                                                                                std::string indent = open_part_np ? "      " : "    ";
                                                                                                                                                                                                                                                                ncx << indent << "<navPoint id=\"navPoint-" << np_counter << "\" playOrder=\"" << play_order << "\">\n"
                                                                                                                                                                                                                                                                << indent << "  <navLabel><text>" << xml_escape(it.label) << "</text></navLabel>\n"
                                                                                                                                                                                                                                                                << indent << "  <content src=\"" << xml_escape(it.href) << "\" />\n"
                                                                                                                                                                                                                                                                << indent << "</navPoint>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (open_part_np) {
                                                                                                                                                                                                                                                                ncx << "    </navPoint>\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                ncx << "  </navMap>\n"
                                                                                                                                                                                                                                                                << "</ncx>\n";
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/toc.ncx", ncx.str());

                                                                                                                                                                                                                                                                std::ostringstream opf;
                                                                                                                                                                                                                                                                opf << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                                                                                                                                                                                                                                                << "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"BookId\">\n"
                                                                                                                                                                                                                                                                << "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                                                                                                                                                                                                                                                                << "    <dc:identifier id=\"BookId\">" << xml_escape(book_id) << "</dc:identifier>\n"
                                                                                                                                                                                                                                                                << "    <dc:title>" << xml_escape(book.title) << "</dc:title>\n";
                                                                                                                                                                                                                                                                if (!book.author.empty()) {
                                                                                                                                                                                                                                                                opf << "    <dc:creator>" << xml_escape(book.author) << "</dc:creator>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                if (!book.date.empty()) {
                                                                                                                                                                                                                                                                opf << "    <dc:date>" << xml_escape(book.date) << "</dc:date>\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                opf << "    <dc:language>ru</dc:language>\n"
                                                                                                                                                                                                                                                                << "    <meta property=\"dcterms:modified\">" << modified_utc << "</meta>\n"
                                                                                                                                                                                                                                                                << "  </metadata>\n"
                                                                                                                                                                                                                                                                << "  <manifest>\n"
                                                                                                                                                                                                                                                                << "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\" />\n"
                                                                                                                                                                                                                                                                << "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\" />\n"
                                                                                                                                                                                                                                                                << "    <item id=\"toc\" href=\"toc.xhtml\" media-type=\"application/xhtml+xml\" />\n"
                                                                                                                                                                                                                                                                << "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\" />\n";

                                                                                                                                                                                                                                                                for (const auto& it : items) {
                                                                                                                                                                                                                                                                opf << "    <item id=\"" << it.id << "\" href=\"" << it.href
                                                                                                                                                                                                                                                                << "\" media-type=\"application/xhtml+xml\" />\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                for (const auto& img : book.images) {
                                                                                                                                                                                                                                                                opf << "    <item id=\"" << img.id << "\" href=\"" << xml_escape(img.epub_rel_href)
                                                                                                                                                                                                                                                                << "\" media-type=\"" << img.media_type << "\" />\n";
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                opf << "  </manifest>\n"
                                                                                                                                                                                                                                                                << "  <spine toc=\"ncx\">\n";

                                                                                                                                                                                                                                                                if (!items.empty()) {
                                                                                                                                                                                                                                                                opf << "    <itemref idref=\"" << items[0].id << "\" />\n";
                                                                                                                                                                                                                                                                opf << "    <itemref idref=\"toc\" />\n";
                                                                                                                                                                                                                                                                for (size_t idx = 1; idx < items.size(); ++idx) {
                                                                                                                                                                                                                                                                opf << "    <itemref idref=\"" << items[idx].id << "\" />\n";
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                opf << "  </spine>\n"
                                                                                                                                                                                                                                                                << "</package>\n";
                                                                                                                                                                                                                                                                write_file(out_dir / "OEBPS/content.opf", opf.str());
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static std::string shell_quote(const std::string& s) {
                                                                                                                                                                                                                                                                std::string q = "'";
                                                                                                                                                                                                                                                                for (char c : s) {
                                                                                                                                                                                                                                                                if (c == '\'') {
                                                                                                                                                                                                                                                                q += "'\\''";
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                q.push_back(c);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                q.push_back('\'');
                                                                                                                                                                                                                                                                return q;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static void pack_epub_zip(const fs::path& src_dir, const fs::path& out_epub) {
                                                                                                                                                                                                                                                                fs::path abs_src = fs::weakly_canonical(src_dir);
                                                                                                                                                                                                                                                                fs::path parent = out_epub.has_parent_path() ? fs::weakly_canonical(out_epub.parent_path()) : fs::current_path();
                                                                                                                                                                                                                                                                fs::path abs_out = parent / out_epub.filename();
                                                                                                                                                                                                                                                                fs::path tmp_out = abs_out;
                                                                                                                                                                                                                                                                tmp_out += ".tmp";

                                                                                                                                                                                                                                                                std::error_code ec;
                                                                                                                                                                                                                                                                fs::remove(tmp_out, ec);
                                                                                                                                                                                                                                                                fs::remove(abs_out, ec);

                                                                                                                                                                                                                                                                std::string q_src = shell_quote(abs_src.string());
                                                                                                                                                                                                                                                                std::string q_tmp = shell_quote(tmp_out.string());

                                                                                                                                                                                                                                                                std::string cmd1 = "cd " + q_src + " && zip -q -X0 " + q_tmp + " mimetype";
                                                                                                                                                                                                                                                                if (std::system(cmd1.c_str()) != 0) {
                                                                                                                                                                                                                                                                fs::remove(tmp_out, ec);
                                                                                                                                                                                                                                                                throw std::runtime_error("Ошибка упаковки mimetype через zip (проверьте, установлен ли пакет zip)");
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                std::string cmd2 = "cd " + q_src + " && zip -q -Xr9 " + q_tmp + " META-INF OEBPS -x \"*.DS_Store\"";
                                                                                                                                                                                                                                                                if (std::system(cmd2.c_str()) != 0) {
                                                                                                                                                                                                                                                                fs::remove(tmp_out, ec);
                                                                                                                                                                                                                                                                throw std::runtime_error("Ошибка упаковки META-INF и OEBPS через zip");
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                fs::rename(tmp_out, abs_out);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                int main(int argc, char* argv[]) {
                                                                                                                                                                                                                                                                if (argc < 2) {
                                                                                                                                                                                                                                                                std::cerr << "Использование: " << argv[0] << " <input.tex>\n";
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                try {
                                                                                                                                                                                                                                                                fs::path main_tex = fs::weakly_canonical(argv[1]);
                                                                                                                                                                                                                                                                fs::path base_dir = main_tex.parent_path();
                                                                                                                                                                                                                                                                std::string stem = main_tex.stem().string();
                                                                                                                                                                                                                                                                if (stem.empty()) {
                                                                                                                                                                                                                                                                throw std::runtime_error("Некорректное имя входного файла: " + main_tex.string());
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                fs::path out_dir = base_dir / stem;
                                                                                                                                                                                                                                                                fs::path out_epub = base_dir / (stem + ".epub");

                                                                                                                                                                                                                                                                std::vector<fs::path> stack{main_tex};
                                                                                                                                                                                                                                                                std::string raw = read_file(main_tex);
                                                                                                                                                                                                                                                                std::string expanded = expand_inputs(raw, base_dir, base_dir, stack);
                                                                                                                                                                                                                                                                std::string cleaned = strip_unwanted_macros(expanded);

                                                                                                                                                                                                                                                                Book book = parse_book(cleaned);
                                                                                                                                                                                                                                                                emit_epub_dir(book, out_dir);
                                                                                                                                                                                                                                                                pack_epub_zip(out_dir, out_epub);

                                                                                                                                                                                                                                                                std::cout << "Каталог EPUB: " << out_dir.string() << "\n";
                                                                                                                                                                                                                                                                std::cout << "Готовый файл: " << out_epub.string() << "\n";
                                                                                                                                                                                                                                                                } catch (const std::exception& ex) {
                                                                                                                                                                                                                                                                std::cerr << "Ошибка: " << ex.what() << "\n";
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                return 0;
                                                                                                                                                                                                                                                                }
