// Minimal, allocation-light JSON reader.
// Parses into a flat node vector of string_views into the source buffer.
// The source buffer must outlive the Doc. Strings are unescaped on demand.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace json {

enum class T : uint8_t { Null, Bool, Num, Str, Arr, Obj };

struct Node {
  T type = T::Null;
  bool b = false;
  double num = 0;
  std::string_view key;   // raw key (escapes kept) when inside an object
  std::string_view str;   // raw string contents (escapes kept)
  uint32_t first = 0;     // first child index (0 = none)
  uint32_t next = 0;      // next sibling index (0 = none)
};

class Doc;

class V {
 public:
  V() = default;
  V(const Doc* d, uint32_t i) : d_(d), i_(i) {}
  bool ok() const { return d_ != nullptr; }
  T type() const;
  bool isNum() const { return ok() && type() == T::Num; }
  bool isStr() const { return ok() && type() == T::Str; }
  bool isObj() const { return ok() && type() == T::Obj; }
  bool isArr() const { return ok() && type() == T::Arr; }
  bool isNull() const { return !ok() || type() == T::Null; }
  V operator[](std::string_view k) const;   // object member (missing -> invalid V)
  V at(size_t idx) const;                   // array element
  size_t size() const;
  double num(double def = 0) const;          // numbers, or numeric strings
  bool boolean(bool def = false) const;
  std::string str(std::string def = {}) const;  // unescaped
  std::string_view rawStr() const;
  // Iteration over children.
  struct It {
    const Doc* d; uint32_t i;
    V operator*() const { return V(d, i); }
    It& operator++();
    bool operator!=(const It& o) const { return i != o.i; }
  };
  It begin() const;
  It end() const { return {d_, 0}; }
  std::string key() const;

 private:
  const Doc* d_ = nullptr;
  uint32_t i_ = 0;
};

class Doc {
 public:
  explicit Doc(std::string_view src) : s_(src) {
    nodes_.reserve(64);
    nodes_.emplace_back();  // index 0 reserved as "none"
    p_ = 0;
    ws();
    if (p_ < s_.size()) root_ = parse(0);
    if (err_) root_ = 0;
  }
  V root() const { return root_ ? V(this, root_) : V(); }
  bool ok() const { return root_ != 0; }
  const Node& n(uint32_t i) const { return nodes_[i]; }

 private:
  std::string_view s_;
  size_t p_ = 0;
  bool err_ = false;
  uint32_t root_ = 0;
  std::vector<Node> nodes_;

  void ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) ++p_;
  }
  bool lit(std::string_view w) {
    if (s_.substr(p_, w.size()) == w) { p_ += w.size(); return true; }
    return false;
  }
  bool rawString(std::string_view& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return false;
    size_t st = ++p_;
    while (p_ < s_.size() && s_[p_] != '"') p_ += (s_[p_] == '\\') ? 2 : 1;
    if (p_ >= s_.size()) return false;
    out = s_.substr(st, p_ - st);
    ++p_;
    return true;
  }
  uint32_t parse(int depth) {
    if (depth > 64 || p_ >= s_.size()) { err_ = true; return 0; }
    uint32_t idx = (uint32_t)nodes_.size();
    nodes_.emplace_back();
    char c = s_[p_];
    if (c == '{' || c == '[') {
      bool obj = c == '{';
      nodes_[idx].type = obj ? T::Obj : T::Arr;
      ++p_; ws();
      uint32_t prev = 0;
      if (p_ < s_.size() && s_[p_] == (obj ? '}' : ']')) { ++p_; return idx; }
      while (!err_) {
        std::string_view k;
        if (obj) {
          ws();
          if (!rawString(k)) { err_ = true; return 0; }
          ws();
          if (p_ >= s_.size() || s_[p_] != ':') { err_ = true; return 0; }
          ++p_;
        }
        ws();
        uint32_t ch = parse(depth + 1);
        if (err_) return 0;
        nodes_[ch].key = k;
        if (prev) nodes_[prev].next = ch; else nodes_[idx].first = ch;
        prev = ch;
        ws();
        if (p_ >= s_.size()) { err_ = true; return 0; }
        if (s_[p_] == ',') { ++p_; continue; }
        if (s_[p_] == (obj ? '}' : ']')) { ++p_; break; }
        err_ = true; return 0;
      }
      return idx;
    }
    if (c == '"') {
      std::string_view v;
      if (!rawString(v)) { err_ = true; return 0; }
      nodes_[idx].type = T::Str; nodes_[idx].str = v;
      return idx;
    }
    if (lit("true")) { nodes_[idx].type = T::Bool; nodes_[idx].b = true; return idx; }
    if (lit("false")) { nodes_[idx].type = T::Bool; return idx; }
    if (lit("null")) return idx;
    // number
    size_t st = p_;
    while (p_ < s_.size() && (isdigit((unsigned char)s_[p_]) || s_[p_] == '-' || s_[p_] == '+' ||
                              s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E')) ++p_;
    if (st == p_) { err_ = true; return 0; }
    std::string tmp(s_.substr(st, p_ - st));
    nodes_[idx].type = T::Num;
    nodes_[idx].num = strtod(tmp.c_str(), nullptr);
    return idx;
  }
};

inline std::string unescape(std::string_view r) {
  std::string o;
  o.reserve(r.size());
  for (size_t i = 0; i < r.size(); ++i) {
    char c = r[i];
    if (c != '\\' || i + 1 >= r.size()) { o += c; continue; }
    char e = r[++i];
    switch (e) {
      case 'n': o += '\n'; break;
      case 't': o += '\t'; break;
      case 'r': o += '\r'; break;
      case 'b': o += '\b'; break;
      case 'f': o += '\f'; break;
      case 'u': {
        if (i + 4 >= r.size()) break;
        unsigned cp = (unsigned)strtoul(std::string(r.substr(i + 1, 4)).c_str(), nullptr, 16);
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < r.size() && r[i + 1] == '\\' && r[i + 2] == 'u') {
          unsigned lo = (unsigned)strtoul(std::string(r.substr(i + 3, 4)).c_str(), nullptr, 16);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        break;
      }
      default: o += e;
    }
  }
  return o;
}

inline T V::type() const { return d_->n(i_).type; }
inline V V::operator[](std::string_view k) const {
  if (!isObj()) return {};
  for (uint32_t c = d_->n(i_).first; c; c = d_->n(c).next)
    if (d_->n(c).key == k) return V(d_, c);
  return {};
}
inline V V::at(size_t idx) const {
  if (!isArr()) return {};
  uint32_t c = d_->n(i_).first;
  while (c && idx--) c = d_->n(c).next;
  return c ? V(d_, c) : V();
}
inline size_t V::size() const {
  if (!ok()) return 0;
  size_t s = 0;
  for (uint32_t c = d_->n(i_).first; c; c = d_->n(c).next) ++s;
  return s;
}
inline double V::num(double def) const {
  if (!ok()) return def;
  const Node& n = d_->n(i_);
  if (n.type == T::Num) return n.num;
  if (n.type == T::Str && !n.str.empty()) {
    std::string t(n.str);
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (end && *end == 0) return v;
  }
  return def;
}
inline bool V::boolean(bool def) const {
  if (!ok() || d_->n(i_).type != T::Bool) return def;
  return d_->n(i_).b;
}
inline std::string V::str(std::string def) const {
  if (!isStr()) return def;
  std::string_view r = d_->n(i_).str;
  return r.find('\\') == std::string_view::npos ? std::string(r) : unescape(r);
}
inline std::string_view V::rawStr() const { return isStr() ? d_->n(i_).str : std::string_view(); }
inline V::It V::begin() const { return {d_, ok() ? d_->n(i_).first : 0u}; }
inline V::It& V::It::operator++() { i = d->n(i).next; return *this; }
inline std::string V::key() const { return ok() ? unescape(d_->n(i_).key) : std::string(); }

}  // namespace json
