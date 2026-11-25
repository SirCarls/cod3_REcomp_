// menukit - reading assets/menus/<game>/*.toml. [waw-decomp/menus1] 2026-11-23
//
// A small reader for the part of TOML the menu files use, so the core needs no library and builds in every port:
//   # comments, [table], [[array of tables]], key = "string", key = [ "a", { row = "b", only = "c" }, ],
//   arrays over several lines, trailing commas, \" \\ \n \t inside strings.
// Anything else is reported as a problem with its line number and the line is skipped.

#include "menukit.h"

#include <utility>

namespace menukit {

bool RowProvider::Step(std::string_view, const Context&, int) { return false; }
bool RowProvider::SetNumber(std::string_view, const Context&, double) { return false; }
bool RowProvider::Activate(std::string_view, const Context&) { return false; }
bool RowProvider::Reset(std::string_view, const Context&) { return false; }
bool Adapter::Condition(std::string_view, const Context&) const { return true; }
void Adapter::Play(Sound, const Context&) {}

namespace {

void Problem(std::vector<std::string>* problems, std::string text) {
  for (const std::string& p : *problems)
    if (p == text) return;
  problems->push_back(std::move(text));
}

// A value: a string, an array of values or an inline table of string fields.
struct Value {
  enum class Type : uint8_t { kString, kArray, kTable } type = Type::kString;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, std::string>> fields;

  std::string Field(std::string_view key) const {
    for (const auto& f : fields)
      if (f.first == key) return f.second;
    return {};
  }
};

// One [table] or [[table]] of the file, with its keys in order.
struct Section {
  std::string name;
  int line = 0;
  std::vector<std::pair<std::string, Value>> keys;

  const Value* Find(std::string_view key) const {
    for (const auto& k : keys)
      if (k.first == key) return &k.second;
    return nullptr;
  }
  std::string Text(std::string_view key) const {
    const Value* v = Find(key);
    return v && v->type == Value::Type::kString ? v->text : std::string();
  }
};

class Reader {
 public:
  Reader(std::string_view text, std::string_view source, std::vector<std::string>* problems)
      : text_(text), source_(source), problems_(problems) {}

  std::vector<Section> ReadAll() {
    std::vector<Section> sections(1);  // keys before any header go to an unnamed section
    while (SkipBlank(), pos_ < text_.size()) {
      if (Peek() == '[') {
        Section s;
        s.line = line_;
        if (ReadHeader(&s.name)) sections.push_back(std::move(s));
      } else {
        std::string key;
        Value value;
        if (ReadKeyValue(&key, &value)) sections.back().keys.emplace_back(std::move(key), std::move(value));
      }
    }
    return sections;
  }

 private:
  char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void Next() {
    if (Peek() == '\n') ++line_;
    ++pos_;
  }
  void SkipSpaces() {
    while (Peek() == ' ' || Peek() == '\t' || Peek() == '\r') Next();
  }
  // Spaces, line ends and comments.
  void SkipBlank() {
    for (;;) {
      SkipSpaces();
      if (Peek() == '#') {
        while (pos_ < text_.size() && Peek() != '\n') Next();
      } else if (Peek() == '\n') {
        Next();
      } else {
        return;
      }
    }
  }
  void Fail(std::string_view what) {
    Problem(problems_, std::string(source_) + ":" + std::to_string(line_) + ": " + std::string(what));
    while (pos_ < text_.size() && Peek() != '\n') Next();  // skip the rest of the line
  }
  // After a value: only spaces and a comment may follow on the line.
  bool EndOfLine() {
    SkipSpaces();
    if (Peek() == '#') {
      while (pos_ < text_.size() && Peek() != '\n') Next();
    }
    if (pos_ >= text_.size() || Peek() == '\n') return true;
    Fail("unexpected text after the value");
    return false;
  }
  static bool KeyChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  }
  bool ReadKey(std::string* key) {
    while (KeyChar(Peek())) {
      key->push_back(Peek());
      Next();
    }
    return !key->empty();
  }
  bool ReadHeader(std::string* name) {
    Next();  // '['
    const bool array = Peek() == '[';
    if (array) Next();
    SkipSpaces();
    if (!ReadKey(name)) {
      Fail("a [table] without a name");
      return false;
    }
    SkipSpaces();
    for (int i = 0; i < (array ? 2 : 1); ++i) {
      if (Peek() != ']') {
        Fail("a [table] header without its closing ]");
        return false;
      }
      Next();
    }
    if (array) name->insert(0, "[]");  // "[]page" marks an array of tables
    return EndOfLine();
  }
  bool ReadKeyValue(std::string* key, Value* value) {
    if (!ReadKey(key)) {
      Fail("expected a key, a [table] or a comment");
      return false;
    }
    SkipSpaces();
    if (Peek() != '=') {
      Fail("expected = after the key " + *key);
      return false;
    }
    Next();
    SkipSpaces();
    return ReadValue(value) && EndOfLine();
  }
  bool ReadString(std::string* out) {
    Next();  // '"'
    while (pos_ < text_.size() && Peek() != '"' && Peek() != '\n') {
      char c = Peek();
      if (c == '\\') {
        Next();
        const char e = Peek();
        c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        if (e != 'n' && e != 't' && e != '"' && e != '\\') {
          Fail("an unknown escape in a string");
          return false;
        }
      }
      out->push_back(c);
      Next();
    }
    if (Peek() != '"') {
      Fail("a string without its closing quote");
      return false;
    }
    Next();
    return true;
  }
  bool ReadValue(Value* value) {
    if (Peek() == '"') {
      value->type = Value::Type::kString;
      return ReadString(&value->text);
    }
    if (Peek() == '[') return ReadArray(value);
    if (Peek() == '{') return ReadTable(value);
    Fail("values are \"strings\", [arrays] or { inline tables }");
    return false;
  }
  bool ReadArray(Value* value) {
    value->type = Value::Type::kArray;
    Next();  // '['
    for (;;) {
      SkipBlank();
      if (Peek() == ']') {
        Next();
        return true;
      }
      Value item;
      if (!ReadValue(&item)) return false;
      value->items.push_back(std::move(item));
      SkipBlank();
      if (Peek() == ',') {
        Next();
      } else if (Peek() != ']') {
        Fail("expected , or ] in an array");
        return false;
      }
    }
  }
  bool ReadTable(Value* value) {
    value->type = Value::Type::kTable;
    Next();  // '{'
    for (;;) {
      SkipSpaces();
      if (Peek() == '}') {
        Next();
        return true;
      }
      std::string key, text;
      if (!ReadKey(&key)) {
        Fail("expected a key in an inline table");
        return false;
      }
      SkipSpaces();
      if (Peek() != '=') {
        Fail("expected = in an inline table");
        return false;
      }
      Next();
      SkipSpaces();
      if (Peek() != '"' || !ReadString(&text)) {
        if (Peek() != '"') Fail("inline table values are \"strings\"");
        return false;
      }
      value->fields.emplace_back(std::move(key), std::move(text));
      SkipSpaces();
      if (Peek() == ',') {
        Next();
      } else if (Peek() != '}') {
        Fail("expected , or } in an inline table");
        return false;
      }
    }
  }

  std::string_view text_, source_;
  std::vector<std::string>* problems_;
  size_t pos_ = 0;
  int line_ = 1;
};

// An `only` list ("cod3mp", or "sp mp" for either): true when this program answers to one of its tags.
bool OnlyHolds(const std::string& only, Adapter& adapter) {
  if (only.empty()) return true;
  size_t start = 0;
  while (start < only.size()) {
    size_t end = only.find(' ', start);
    if (end == std::string::npos) end = only.size();
    if (end > start && adapter.HasTag(std::string_view(only).substr(start, end - start))) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace

bool FindRow(Adapter& adapter, std::string_view id, RowProvider** provider, std::string_view* name) {
  const size_t colon = id.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= id.size()) return false;
  *provider = adapter.Provider(id.substr(0, colon));
  *name = id.substr(colon + 1);
  return *provider != nullptr;
}

bool ParseMenu(std::string_view text, std::string_view source, Adapter& adapter, Menu* out,
               std::vector<std::string>* problems) {
  Reader reader(text, source, problems);
  const std::vector<Section> sections = reader.ReadAll();
  const std::string where(source);
  bool have_menu = false;
  for (const Section& s : sections) {
    if (s.name == "menu") {
      have_menu = true;
      out->id = s.Text("id");
      out->title_key = s.Text("title");
      out->takes_over = s.Text("takes_over");
      out->back = s.Text("back");
      if (out->back.empty()) out->back = "game";
      if (out->id.empty()) Problem(problems, where + ": [menu] has no id");
      continue;
    }
    if (s.name != "[]page") {
      if (!s.name.empty())
        Problem(problems, where + ":" + std::to_string(s.line) + ": unknown section [" + s.name + "]");
      else if (!s.keys.empty())
        Problem(problems, where + ": keys before any [table] are not used");
      continue;
    }
    Page page;
    page.id = s.Text("id");
    page.label_key = s.Text("label");
    page.title_key = s.Text("title");
    const std::string at = where + " page " + page.id;
    if (page.id.empty()) Problem(problems, where + ":" + std::to_string(s.line) + ": a [[page]] without an id");
    if (!OnlyHolds(s.Text("only"), adapter)) continue;  // another program's page
    const Value* rows = s.Find("rows");
    if (!rows || rows->type != Value::Type::kArray) {
      Problem(problems, at + ": no rows = [ ... ]");
      continue;
    }
    for (const Value& item : rows->items) {
      Row row;
      std::string only;
      if (item.type == Value::Type::kString) {
        row.id = item.text;
      } else if (item.type == Value::Type::kTable) {
        row.id = item.Field("row");
        row.note_key = item.Field("note");
        row.when = item.Field("when");
        only = item.Field("only");
      }
      if (row.id.empty()) {
        Problem(problems, at + ": a row without a name");
        continue;
      }
      if (!OnlyHolds(only, adapter)) continue;  // another program's row
      RowProvider* provider = nullptr;
      std::string_view name;
      RowView view;
      if (!FindRow(adapter, row.id, &provider, &name) || !provider->View(name, Context{}, &view)) {
        Problem(problems, at + ": " + row.id + " isn't a row " + std::string(adapter.Game()) + " has; left out");
        continue;
      }
      page.rows.push_back(std::move(row));
    }
    if (page.rows.empty()) {
      Problem(problems, at + ": no rows in this program; the page is left out");
      continue;
    }
    out->pages.push_back(std::move(page));
  }
  if (!have_menu) Problem(problems, where + ": no [menu] section");
  return have_menu && !out->pages.empty();
}

}  // namespace menukit
