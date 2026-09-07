#include "BookMutationJson.h"

#include <cstdio>
#include <cstring>

#include "BookMutationOwners.h"
namespace bookmutation {
namespace {
class Rewrite {
  FsFile& in;
  FsFile& out;
  JsonScratch& w;
  RewritePath callback;
  void* context;
  size_t inputPos = 0, inputSize = 0, outputSize = 0;
  uint64_t consumed = 0;
  bool emit = true, ok = true;
  JsonKind kind;
  size_t knownBytes = 0;
  int peek() {
    if (inputPos == inputSize) {
      serviceMutation();
      int n = in.read(w.input, sizeof(w.input));
      if (n < 0) {
        ok = false;
        return -1;
      }
      inputSize = n;
      inputPos = 0;
    }
    return inputPos < inputSize ? w.input[inputPos] : -1;
  }
  bool put(char c) {
    if (!emit) return true;
    w.output[outputSize++] = uint8_t(c);
    if (outputSize == sizeof(w.output)) return flush();
    return true;
  }
  int get() {
    int c = peek();
    if (c >= 0) {
      ++inputPos;
      ++consumed;
      if (!put(char(c))) ok = false;
    }
    return c;
  }
  bool flush() {
    serviceMutation();
    if (outputSize && out.write(w.output, outputSize) != outputSize) ok = false;
    outputSize = 0;
    return ok;
  }
  bool literal(const char* value) {
    while (*value)
      if (get() != *value++) return false;
    return ok;
  }
  void ws() {
    while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') get();
  }
  bool punctuation(int c) {
    ws();
    return get() == c && ok;
  }
  bool seek(uint64_t pos) {
    inputPos = inputSize = 0;
    consumed = pos;
    return in.seek64(pos);
  }
  bool string(char* capture = nullptr, size_t capacity = 0, bool strict = true) {
    if (get() != '"') return false;
    size_t n = 0;
    bool overflow = false;
    auto add = [&](uint32_t ch) {
      if (!capture) return;
      if (n + 1 < capacity)
        capture[n++] = char(ch);
      else
        overflow = true;
    };
    while (true) {
      int c = get();
      if (c < 0 || c < 32) return false;
      if (c == '"') break;
      if (c == '\\') {
        c = get();
        if (c == 'u') {
          uint32_t value = 0;
          for (unsigned i = 0; i < 4; ++i) {
            int h = get();
            int d = h >= '0' && h <= '9'   ? h - '0'
                    : h >= 'a' && h <= 'f' ? h - 'a' + 10
                    : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                           : -1;
            if (d < 0) return false;
            value = value * 16 + d;
          }
          if (value >= 0xd800 && value <= 0xdfff) {
            if (value > 0xdbff || get() != '\\' || get() != 'u') return false;
            uint32_t low = 0;
            for (unsigned i = 0; i < 4; ++i) {
              int h = get();
              int d = h >= '0' && h <= '9'   ? h - '0'
                      : h >= 'a' && h <= 'f' ? h - 'a' + 10
                      : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                             : -1;
              if (d < 0) return false;
              low = low * 16 + d;
            }
            if (low < 0xdc00 || low > 0xdfff) return false;
            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
          }
          if (capture && value == 0) return false;
          if (value < 0x80)
            add(value);
          else if (value < 0x800) {
            add(0xc0 | (value >> 6));
            add(0x80 | (value & 63));
          } else if (value < 0x10000) {
            add(0xe0 | (value >> 12));
            add(0x80 | ((value >> 6) & 63));
            add(0x80 | (value & 63));
          } else {
            add(0xf0 | (value >> 18));
            add(0x80 | ((value >> 12) & 63));
            add(0x80 | ((value >> 6) & 63));
            add(0x80 | (value & 63));
          }
          continue;
        }
        switch (c) {
          case '"':
          case '\\':
          case '/':
            break;
          case 'b':
            c = 8;
            break;
          case 'f':
            c = 12;
            break;
          case 'n':
            c = 10;
            break;
          case 'r':
            c = 13;
            break;
          case 't':
            c = 9;
            break;
          default:
            return false;
        }
      }
      add(c);
    }
    if (capture) {
      capture[n] = 0;
      if (overflow && !strict) capture[0] = 0;
    }
    return ok && (!strict || !overflow);
  }
  bool quoted(const char* s) {
    if (!put('"')) return false;
    while (*s) {
      const unsigned char c = *s++;
      if (c == '"' || c == '\\') {
        put('\\');
        put(c);
      } else if (c < 32) {
        char escape[7];
        snprintf(escape, sizeof(escape), "\\u%04x", c);
        for (const char* p = escape; *p; ++p) put(*p);
      } else
        put(c);
    }
    return put('"') && ok;
  }
  bool number() {
    if (peek() == '-') get();
    int c = peek();
    if (c == '0')
      get();
    else {
      if (c < '1' || c > '9') return false;
      while (peek() >= '0' && peek() <= '9') get();
    }
    if (peek() == '.') {
      get();
      if (peek() < '0' || peek() > '9') return false;
      while (peek() >= '0' && peek() <= '9') get();
    }
    if (peek() == 'e' || peek() == 'E') {
      get();
      if (peek() == '+' || peek() == '-') get();
      if (peek() < '0' || peek() > '9') return false;
      while (peek() >= '0' && peek() <= '9') get();
    }
    return ok;
  }
  bool value(unsigned depth) {
    if (depth > 8) return false;
    ws();
    int c = peek();
    if (c == '"') return string();
    if (c == '{' || c == '[') {
      get();
      const int close = c == '{' ? '}' : ']';
      ws();
      if (peek() == close) {
        get();
        return ok;
      }
      do {
        if (c == '{' && (!string() || !punctuation(':'))) return false;
        if (!value(depth + 1)) return false;
        ws();
        if (peek() == close) {
          get();
          return ok;
        }
        if (get() != ',') return false;
        ws();
      } while (ok);
      return false;
    }
    if (c == 't') return literal("true");
    if (c == 'f') return literal("false");
    if (c == 'n') return literal("null");
    return number();
  }
  bool field(PathField field) {
    const bool saved = emit;
    emit = false;
    bool parsed = string(w.value, sizeof(w.value));
    emit = saved;
    if (!parsed) return false;
    auto edit = callback(context, field, w.book, w.value, w.replacement, sizeof(w.replacement));
    if (edit == PathEdit::Error) return false;
    return quoted(edit == PathEdit::Keep ? w.value : edit == PathEdit::Remove ? "" : w.replacement);
  }
  bool bookObject(bool discover) {
    if (!punctuation('{')) return false;
    ws();
    bool found = false;
    if (peek() == '}') {
      get();
      return false;
    }
    do {
      char key[32];
      if (!string(key, sizeof(key), false) || !punctuation(':')) return false;
      ws();
      if (!strcmp(key, "path")) {
        if (found) return false;
        found = true;
        if (discover) {
          if (!string(w.book, sizeof(w.book))) return false;
          knownBytes += strlen(w.book);
        } else if (!field(PathField::Book))
          return false;
      } else if (discover && (!strcmp(key, "title") || !strcmp(key, "author") || !strcmp(key, "coverBmpPath"))) {
        if (!string(w.value, sizeof(w.value))) return false;
        knownBytes += strlen(w.value);
      } else if (!discover && !strcmp(key, "coverBmpPath")) {
        if (!field(PathField::Cover)) return false;
      } else if (!value(0))
        return false;
      if (knownBytes > 16384) return false;
      ws();
      if (peek() == '}') {
        get();
        return found && ok;
      }
      if (get() != ',') return false;
      ws();
    } while (ok);
    return false;
  }
  bool books() {
    if (!punctuation('[')) return false;
    bool first = true;
    size_t count = 0;
    // Array separators are regenerated so removing a row leaves valid JSON.
    const bool saved = emit;
    emit = false;
    ws();
    while (peek() != ']') {
      if (++count > 18) return false;
      const uint64_t begin = consumed;
      if (!bookObject(true)) return false;
      const uint64_t end = consumed;
      auto edit = callback(context, PathField::Book, w.book, w.book, w.replacement, sizeof(w.replacement));
      if (edit == PathEdit::Error) return false;
      if (edit != PathEdit::Remove) {
        emit = saved;
        if (!first) put(',');
        first = false;
        if (!seek(begin) || !bookObject(false)) return false;
        emit = false;
      } else if (!seek(end))
        return false;
      ws();
      if (peek() == ']') break;
      if (get() != ',') return false;
      ws();
      if (peek() == ']') return false;
    }
    emit = saved;
    return get() == ']' && ok;
  }

 public:
  Rewrite(FsFile& a, FsFile& b, JsonKind k, RewritePath cb, void* c, JsonScratch& scratch)
      : in(a), out(b), w(scratch), callback(cb), context(c), kind(k) {}
  bool run() {
    w.book[0] = 0;
    if (!punctuation('{')) return false;
    ws();
    bool seenBooks = false;
    if (peek() != '}') do {
        char key[32];
        if (!string(key, sizeof(key), false) || !punctuation(':')) return false;
        ws();
        if (kind == JsonKind::Recent && !strcmp(key, "books")) {
          if (seenBooks || !books()) return false;
          seenBooks = true;
        } else if (kind == JsonKind::State && !strcmp(key, "openEpubPath")) {
          if (!field(PathField::Resume)) return false;
        } else if (kind == JsonKind::State && !strcmp(key, "favoriteSleepImagePath")) {
          if (!field(PathField::Favorite)) return false;
        } else if (kind == JsonKind::State && !strcmp(key, "preferredSleepFolderPath")) {
          if (!field(PathField::Preferred)) return false;
        } else if (!value(0))
          return false;
        ws();
        if (peek() == '}') break;
        if (get() != ',') return false;
        ws();
      } while (ok);
    if (get() != '}') return false;
    ws();
    return peek() == -1 && ok && flush() && (kind != JsonKind::Recent || seenBooks);
  }
};
}  // namespace
bool rewriteSharedJson(FsFile& in, FsFile& out, JsonKind kind, RewritePath cb, void* context, JsonScratch& scratch) {
  Rewrite rewrite(in, out, kind, cb, context, scratch);
  return rewrite.run();
}
}  // namespace bookmutation
