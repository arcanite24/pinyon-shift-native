// The string-table lookups the engine makes by name: every
// loc_str / str / resolve_setting_loc ("Table", "IDS_Id", ...) call in
// apps/ and libs/ and every table lookup_str names must exist in the
// player's tables (FORMAT-STRINGTABLES: the disc's EN.zip, v4's
// stringtables/en/, then the packages), or the screen silently shows its
// English fallback in every language. The sources are scanned, so a new
// call is checked without listing it here. With --corpus (SHIFT_GAME_ROOT
// and SHIFT_STATE_ROOT; exit 77 without them) the pairs are resolved, and
// the Rally catalog's names come from the tables when given them.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/stringtables.h"
#include "race/rally_championship.h"

#ifndef SHIFT_SOURCE_DIR
#error SHIFT_SOURCE_DIR must name the source tree
#endif

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct Ref {
  std::string table, id;  // id empty: a table-only lookup (lookup_str)
  std::string where;
  bool operator<(const Ref& o) const {
    return table != o.table ? table < o.table : id != o.id ? id < o.id : where < o.where;
  }
};

bool ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void skip_space(std::string_view s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

// A string literal of identifier characters at `i`.
bool literal(std::string_view s, size_t& i, std::string& out) {
  skip_space(s, i);
  if (i >= s.size() || s[i] != '"') return false;
  size_t j = i + 1;
  while (j < s.size() && ident(s[j])) ++j;
  if (j >= s.size() || s[j] != '"' || j == i + 1) return false;
  out.assign(s.substr(i + 1, j - i - 1));
  i = j + 1;
  return true;
}

// The lookups in one source text.
void scan(std::string_view text, const std::string& file, std::vector<Ref>& out) {
  // (table, id) pairs; lookup_str(tables, "table", id) and
  // string_hash("table") << 16 | ... name the table alone.
  enum Kind { kPair, kLookupStr, kTableHash };
  static constexpr struct {
    const char* name;
    Kind kind;
  } kCalls[] = {{"loc_str", kPair},
                {"str", kPair},
                {"resolve_setting_loc", kPair},
                {"lookup_str", kLookupStr},
                {"string_hash", kTableHash}};
  for (size_t at = 0; at < text.size(); ++at) {
    if (at > 0 && (ident(text[at - 1]) || text[at - 1] == '.' || text[at - 1] == '>')) continue;
    std::string_view fn;
    Kind kind = kPair;
    for (const auto& c : kCalls) {
      const size_t n = std::strlen(c.name);
      if (text.compare(at, n, c.name) == 0 && (at + n >= text.size() || !ident(text[at + n]))) {
        fn = c.name;
        kind = c.kind;
      }
    }
    if (fn.empty()) continue;
    size_t i = at + fn.size();
    skip_space(text, i);
    if (i >= text.size() || text[i] != '(') continue;
    ++i;
    const size_t line = size_t(std::count(text.begin(), text.begin() + std::ptrdiff_t(at), '\n')) + 1;
    const std::string where = file + ":" + std::to_string(line);
    Ref r;
    if (kind == kLookupStr) {
      while (i < text.size() && text[i] != ',' && text[i] != ')') ++i;
      if (i >= text.size() || text[i] != ',') continue;
      ++i;
      if (!literal(text, i, r.table)) continue;
    } else if (kind == kTableHash) {
      // Only a table hash: the hashed literal shifted into the high half.
      if (!literal(text, i, r.table)) continue;
      size_t j = i;
      while (j < text.size() && text[j] != '<' && text[j] != ';' && text[j] != '\n') ++j;
      if (text.compare(j, 4, "<< 1") != 0) continue;
    } else {
      if (!literal(text, i, r.table)) continue;
      skip_space(text, i);
      if (i >= text.size() || text[i] != ',') continue;
      ++i;
      if (!literal(text, i, r.id)) continue;
    }
    r.where = where;
    out.push_back(std::move(r));
  }
}

std::vector<Ref> scan_tree(const std::filesystem::path& root) {
  std::vector<Ref> refs;
  for (const char* dir : {"apps", "libs"}) {
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root / dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const auto ext = e.path().extension();
      if (ext != ".cpp" && ext != ".h") continue;
      std::ifstream in(e.path(), std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      scan(text, std::filesystem::relative(e.path(), root).generic_string(), refs);
    }
  }
  return refs;
}

std::string packed_ref(std::string_view table, std::string_view id) {
  const uint32_t n = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  return "_&" + std::to_string(n);
}

void unit(const std::vector<Ref>& refs) {
  std::vector<Ref> got;
  scan("x = loc_str(\"InGame\", \"IDS_Progress\", \"PROGRESS\");\n"
       "y = str(\n  \"HelpButtons\",\n  \"IDS_Back\", \"BACK\");\n"
       "ss.str(\"\"); a.str(\"T\", \"IDS_X\"); my_str(\"T\", \"IDS_Y\");\n"
       "lookup_str(strings, \"carchallenges\", title);\n"
       "loc_str(table, \"IDS_Z\", \"\");\n"
       "k = uint32_t(fh1::string_hash(\"Barnfind\")) << 16 | fh1::string_hash(key);\n"
       "h = fh1::string_hash(\"IDS_NotATable\");\n",
       "t.cpp", got);
  check(got.size() == 4, "the scanner finds the four lookups");
  if (got.size() == 4) {
    check(got[0].table == "InGame" && got[0].id == "IDS_Progress" && got[0].where == "t.cpp:1", "first pair");
    check(got[1].table == "HelpButtons" && got[1].id == "IDS_Back" && got[1].where == "t.cpp:2", "multi-line pair");
    check(got[2].table == "carchallenges" && got[2].id.empty(), "table-only lookup_str");
    check(got[3].table == "Barnfind" && got[3].id.empty() && got[3].where == "t.cpp:8", "table hash");
  }

  // The packed reference resolves through StringTables as the screens build it.
  fh1::StringTables st;
  fh1::StringTable t;
  t.keys = {fh1::string_hash("IDS_Progress")};
  t.text = {u"PROGRESS"};
  st.put("InGame", t);
  check(st.has("ingame") && !st.has("Career"), "has() by name, any case");
  const auto r = st.resolve(packed_ref("InGame", "IDS_Progress"));
  check(r && *r == "PROGRESS", "packed reference resolves");
  check(!st.resolve(packed_ref("Career", "IDS_Progress")), "a missing table does not resolve");

  // The tree has lookups to check (the scan works on this checkout).
  size_t pairs = 0;
  for (const auto& ref : refs) pairs += !ref.id.empty();
  std::printf("string_refs: %zu lookups in the sources (%zu pairs)\n", refs.size(), pairs);
  check(pairs > 100, "the sources hold the engine's lookups");
}

int corpus(const std::vector<Ref>& refs, const std::filesystem::path& game_root,
           const std::filesystem::path& state_root) {
  cars::Install inst;
  std::string err;
  if (!cars::discover_install(game_root, state_root, {}, inst, &err)) {
    std::fprintf(stderr, "FAIL: install: %s\n", err.c_str());
    return 1;
  }
  check(!inst.v4_media_zip.empty(), "the state root holds v4");
  fh1::StringTables st;
  cars::StringTableLoad counts;
  check(cars::load_string_tables(inst, st, &err, "EN", &counts), "string tables load");
  std::printf("string_refs corpus: %zu tables (disc %zu, v4 %zu, packages %zu)\n", st.size(), counts.disc,
              counts.v4, counts.packages);

  std::set<std::string> seen;
  int bad = 0, checked = 0;
  for (const auto& r : refs) {
    if (!seen.insert(r.table + "/" + r.id).second) continue;
    ++checked;
    if (r.id.empty()) {
      if (!st.has(r.table)) {
        std::fprintf(stderr, "FAIL: %s: no table %s\n", r.where.c_str(), r.table.c_str());
        ++bad;
      }
      continue;
    }
    const auto text = st.resolve(packed_ref(r.table, r.id));
    if (!text || text->empty()) {
      std::fprintf(stderr, "FAIL: %s: %s %s (%s)\n", r.where.c_str(), r.table.c_str(), r.id.c_str(),
                   st.has(r.table) ? "no such id" : "no such table");
      ++bad;
    }
  }
  std::printf("string_refs corpus: %d distinct lookups, %d unresolved\n", checked, bad);
  check(bad == 0, "every lookup the engine makes resolves");

  // The Rally catalog names its championships and stages by string ref: the
  // tables must change them from the built-in fallbacks.
  race::RallyCatalog plain, named;
  std::string e1, e2;
  check(race::load_rally_catalog(state_root, nullptr, nullptr, plain, &e1), "rally catalog loads without tables");
  check(race::load_rally_catalog(state_root, nullptr, &st, named, &e2), "rally catalog loads with tables");
  check(plain.championships.size() == named.championships.size() && !named.championships.empty(),
        "the same championships either way");
  int champs_named = 0, stages_named = 0, stages = 0;
  for (size_t i = 0; i < named.championships.size() && i < plain.championships.size(); ++i) {
    const auto& a = plain.championships[i];
    const auto& b = named.championships[i];
    check(!b.name.empty() && b.name.rfind("_&", 0) != 0, "championship name is text");
    champs_named += a.name != b.name;
    for (size_t s = 0; s < b.stages.size() && s < a.stages.size(); ++s) {
      ++stages;
      check(!b.stages[s].name.empty() && b.stages[s].name.rfind("_&", 0) != 0, "stage name is text");
      stages_named += a.stages[s].name != b.stages[s].name;
    }
  }
  std::printf("string_refs corpus: rally %d/%zu championships and %d/%d stages named from the tables\n",
              champs_named, named.championships.size(), stages_named, stages);
  check(champs_named > 0, "rally championships take their names from the tables");
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<Ref> refs = scan_tree(SHIFT_SOURCE_DIR);
  if (argc > 1 && std::strcmp(argv[1], "--corpus") == 0) {
    const char* game = std::getenv("SHIFT_GAME_ROOT");
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    if (!game || !*game || !state || !*state) {
      std::printf("string_refs_tests --corpus: SHIFT_GAME_ROOT and SHIFT_STATE_ROOT not set, skipped\n");
      return 77;
    }
    if (corpus(refs, game, state)) {
      std::fprintf(stderr, "string_refs_tests --corpus: %d failure(s)\n", failures);
      return 1;
    }
    std::printf("string_refs_tests --corpus: all passed\n");
    return 0;
  }
  unit(refs);
  if (failures) {
    std::fprintf(stderr, "string_refs_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("string_refs_tests: all passed\n");
  return 0;
}
