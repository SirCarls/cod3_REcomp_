// Standalone check of the settings-file rules in SDK patch 0042 (cvar.cpp):
// a command-line value is a one-off (the file keeps its own value), lines for
// settings nobody registered are kept, and SetDefaultValue makes an app's own
// default the one the file leaves out.
//
// Build against a patched SDK checkout ($SDK) and its built rexruntime
// ($LIB, the install's lib folder), with the SDK's own compiler:
//   clang++ -std=c++23 -DSPDLOG_FMT_EXTERNAL -I$SDK/include -I$SDK/src \
//     -I$SDK/thirdparty/cli11/include -I$SDK/thirdparty/tomlplusplus/include \
//     -I$SDK/thirdparty/spdlog/include -I$SDK/thirdparty/fmt/include \
//     0042-cvar-save-test.cpp $SDK/src/core/cvar.cpp -L$LIB -lrexruntime -lfmt -lspdlog \
//     -Wl,-rpath,$LIB -o cvar_save_test && ./cvar_save_test
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/cvar_file.h>

REXCVAR_DEFINE_BOOL(t_window, true, "Test", "fullscreen-like flag");
REXCVAR_DEFINE_INT32(t_num, 5, "Test", "number");
REXCVAR_DEFINE_STRING(t_text, "a", "Test", "text");
REXCVAR_DEFINE_BOOL(t_port, false, "Test", "app default differs from the SDK's");
REXCVAR_DEFINE_BOOL(t_port2, false, "Test", "app default, file has its own value");
REXCVAR_DEFINE_DOUBLE(t_rate, 60.0, "Test", "double with an app default");
REXCVAR_DEFINE_STRING(t_once, "", "Test", "one-off value only on the command line");

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static bool Has(const std::string& toml, const std::string& line) {
  std::istringstream in(toml);
  for (std::string l; std::getline(in, l);)
    if (l == line) return true;
  return false;
}
static bool HasKey(const std::string& toml, const std::string& key) {
  std::istringstream in(toml);
  for (std::string l; std::getline(in, l);)
    if (l.rfind(key + " = ", 0) == 0) return true;
  return false;
}

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "cvar_save_test";
  fs::create_directories(dir);
  const fs::path file = dir / "app.toml";
  {
    std::ofstream f(file);
    f << "t_num = 7\n"
         "t_text = \"file\"\n"
         "t_port2 = false\n"
         "plugin_setting = \"keep me\"\n"
         "other_number = 3\n";
  }
  const char* argv[] = {"app", "--t_num=9", "--no-t_window", "--t_once=/tmp/launch.bin"};
  rex::cvar::Init(4, const_cast<char**>(argv));
  rex::cvar::ApplyEnvironment();
  rex::cvar::LoadConfig(file);

  int callback_calls = 0;
  rex::cvar::RegisterChangeCallback("t_port", [&](std::string_view, std::string_view) { ++callback_calls; });

  // The command line wins for this run...
  CHECK(REXCVAR_GET(t_num) == 9);
  CHECK(REXCVAR_GET(t_window) == false);
  CHECK(rex::cvar::GetConfigFileValue("t_num") == std::optional<std::string>("7"));
  CHECK(!rex::cvar::GetConfigFileValue("t_window").has_value());

  // App defaults.
  CHECK(rex::cvar::SetDefaultValue("t_port", "true"));
  CHECK(REXCVAR_GET(t_port) == true);  // still at its default: takes the new one
  CHECK(callback_calls == 1);
  CHECK(rex::cvar::SetDefaultValue("t_port2", "true"));
  CHECK(REXCVAR_GET(t_port2) == false);  // the file's value stays
  CHECK(rex::cvar::SetDefaultValue("t_rate", "1000"));
  CHECK(!rex::cvar::SetDefaultValue("no_such_flag", "1"));
  CHECK(!rex::cvar::SetDefaultValue("t_num", "not a number"));

  std::string toml = rex::cvar::SerializeToTOML();
  std::fprintf(stderr, "--- after start:\n%s", toml.c_str());
  // ...but the file keeps its own values for them.
  CHECK(Has(toml, "t_num = 7"));
  CHECK(!HasKey(toml, "t_window"));
  CHECK(!HasKey(toml, "t_once"));
  CHECK(Has(toml, "t_text = \"file\""));
  CHECK(!HasKey(toml, "t_port"));      // at the app's default: no line
  CHECK(Has(toml, "t_port2 = false"));  // the SDK's value, chosen by the player: kept
  CHECK(!HasKey(toml, "t_rate"));      // 1000 == 1000.000000
  CHECK(Has(toml, "plugin_setting = \"keep me\""));
  CHECK(Has(toml, "other_number = 3"));

  // A command-line value changed through REXCVAR_SET (the source stays
  // kCommandLine) is a change made in the app: saved.
  REXCVAR_SET(t_once, std::string("changed in the app"));
  CHECK(Has(rex::cvar::SerializeToTOML(), "t_once = \"changed in the app\""));
  REXCVAR_SET(t_once, std::string("/tmp/launch.bin"));
  CHECK(!HasKey(rex::cvar::SerializeToTOML(), "t_once"));

  // Changes made in the app are saved, over a command-line value too.
  rex::cvar::SetFlagByName("t_num", "11");
  rex::cvar::SetFlagByName("t_window", "true");   // back to the default: no line
  rex::cvar::SetFlagByName("t_port", "false");    // the SDK's value: a line now
  const fs::path out = dir / "saved.toml";
  rex::cvar::SaveConfig(out);
  std::ifstream in(out);
  std::stringstream ss;
  ss << in.rdbuf();
  toml = ss.str();
  std::fprintf(stderr, "--- saved:\n%s", toml.c_str());
  CHECK(Has(toml, "t_num = 11"));
  CHECK(!HasKey(toml, "t_window"));
  CHECK(Has(toml, "t_port = false"));
  CHECK(Has(toml, "plugin_setting = \"keep me\""));
  CHECK(rex::cvar::GetConfigFileValue("t_num") == std::optional<std::string>("11"));

  // A second save in the same run gives the same file.
  CHECK(rex::cvar::SerializeToTOML() + "" == toml.substr(toml.find('\n') + 1));

  // A reset goes to the app's default.
  rex::cvar::ResetToDefault("t_port");
  CHECK(REXCVAR_GET(t_port) == true);

  fs::remove_all(dir);
  std::fprintf(stderr, failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
