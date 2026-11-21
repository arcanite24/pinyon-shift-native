// The player's string tables for one language, merged in FORMAT-STRINGTABLES
// order (Loading and overriding): the disc's media/stringtables/<L>.zip, v4's
// media.zip stringtables/<l>/, then each package's
// Media/DLCZips/StringTables_pri_<q>.zip members <L>/<Table>.str in
// ascending q; a later table replaces an earlier one whole. The packages are
// the ones discover_install found (the community package left out).
#pragma once

#include <filesystem>
#include <string>

#include "cars/sources.h"
#include "fh1data/stringtables.h"

namespace cars {

struct StringTableLoad {
  size_t disc = 0, v4 = 0, packages = 0, rejected = 0;  // tables put, by source
};

// `language`: an archive name, "EN" by default. False only when the disc's
// archive cannot be opened.
bool load_string_tables(const Install& install, fh1::StringTables& out, std::string* error,
                        const std::string& language = "EN", StringTableLoad* counts = nullptr);

}  // namespace cars
