// CPU test for nr_constants32::InspectCompleteSet (G56 b).
//
// Fixtures live only under out/lanes/g56/install-test-tmp and are removed when the test
// finishes. The command-line form `test LEGACY CANDIDATE` checks one existing pair and
// prints Inspector PASS or the inspector's error.
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <system_error>

#include "native/nr/nr_constants32_install.h"

namespace fs = std::filesystem;

namespace {

const char* const kRoot = "out/lanes/g56/install-test-tmp";

const std::string kBaseManifest =
    "main\tvs\tmain.vs.spv\tmain.hlsl\n"
    "ui\tps\tui.ps.spv\tui.hlsl\n"
    "opt\tvs\topt.vs.spv\tnone\n";

int g_checks = 0;
int g_failures = 0;

bool MakeDir(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    std::cerr << "fixture: cannot create " << path << ": " << ec.message() << "\n";
    return false;
  }
  return true;
}

bool WriteFile(const fs::path& path, const std::string& bytes) {
  if (!MakeDir(path.parent_path())) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "fixture: cannot open " << path << " for writing\n";
    return false;
  }
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    std::cerr << "fixture: cannot write " << path << "\n";
    return false;
  }
  return true;
}

bool Remove(const fs::path& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    std::cerr << "fixture: cannot remove " << path << ": " << ec.message() << "\n";
    return false;
  }
  return true;
}

bool ResetManifest(const fs::path& legacy, const fs::path& candidate, const std::string& text) {
  return WriteFile(legacy / "manifest.tsv", text) &&
         WriteFile(candidate / "manifest.tsv", text);
}

// A complete pair: three manifest rows (one without any HLSL), two top-level helper/variant
// SPIR-V files outside the manifest, identical contracts where legacy has them, and
// candidate-only extras that the inspector must ignore.
bool BuildBase(const fs::path& legacy, const fs::path& candidate) {
  if (!ResetManifest(legacy, candidate, kBaseManifest)) return false;
  if (!WriteFile(candidate / "constants32", "")) return false;
  const char* const spv[] = {"main.vs.spv", "ui.ps.spv", "opt.vs.spv", "helper.vs.spv",
                             "variant.ps.spv"};
  for (const char* name : spv) {
    if (!WriteFile(legacy / name, std::string("legacy:") + name)) return false;
    if (!WriteFile(candidate / name, std::string("candidate:") + name)) return false;
  }
  if (!WriteFile(legacy / "main.vs.hlsl", "float4 main_vs() { return 0; }\n")) return false;
  if (!WriteFile(candidate / "main.vs.hlsl", "float4 main_vs() { return 0; }\n")) return false;
  if (!WriteFile(legacy / "ui.ps.hlsl", "float4 ui_ps() { return 0; }\n")) return false;
  if (!WriteFile(candidate / "ui.ps.hlsl", "float4 ui_ps() { return 0; }\n")) return false;
  if (!WriteFile(candidate / "extra.spv", "candidate-only")) return false;
  if (!WriteFile(candidate / "notes.txt", "ignored")) return false;
  return true;
}

void Check(const std::string& name, const nr_constants32::Selection& sel, bool expect_ok,
           const std::string& error_fragment) {
  ++g_checks;
  bool good = sel.ok == expect_ok && sel.enabled == expect_ok;
  if (expect_ok) {
    good = good && sel.error.empty();
  } else {
    good = good && !sel.error.empty() &&
           (error_fragment.empty() || sel.error.find(error_fragment) != std::string::npos);
  }
  std::cout << (good ? "PASS " : "FAIL ") << name;
  if (!good) {
    std::cout << " -- ok=" << sel.ok << " enabled=" << sel.enabled << " error=\"" << sel.error
              << "\"";
    ++g_failures;
  }
  std::cout << "\n";
}

using Mutation = std::function<bool(const fs::path&, const fs::path&)>;

void RunCase(const std::string& name, bool expect_ok, const std::string& error_fragment,
             const Mutation& mutate) {
  const fs::path dir = fs::path(kRoot) / name;
  const fs::path legacy = dir / "legacy";
  const fs::path candidate = dir / "candidate";
  if (!BuildBase(legacy, candidate)) {
    ++g_failures;
    std::cerr << "FAIL " << name << " -- fixture setup failed\n";
    return;
  }
  if (mutate && !mutate(legacy, candidate)) {
    ++g_failures;
    std::cerr << "FAIL " << name << " -- fixture mutation failed\n";
    return;
  }
  Check(name, nr_constants32::InspectCompleteSet(legacy.string(), candidate.string()), expect_ok,
        error_fragment);
}

int RunTests() {
  std::error_code ec;
  fs::remove_all(fs::path(kRoot), ec);
  if (ec) {
    std::cerr << "cannot reset fixture root: " << ec.message() << "\n";
    return 1;
  }
  if (!MakeDir(fs::path(kRoot))) return 1;

  RunCase("complete", true, "", nullptr);

  RunCase("marker_absent", false, "constants32", [](const fs::path&, const fs::path& candidate) {
    return Remove(candidate / "constants32");
  });
  RunCase("marker_directory", false, "constants32",
          [](const fs::path&, const fs::path& candidate) {
            return Remove(candidate / "constants32") && MakeDir(candidate / "constants32");
          });
  RunCase("manifest_empty_legacy", false, "empty",
          [](const fs::path& legacy, const fs::path&) {
            return WriteFile(legacy / "manifest.tsv", "");
          });
  RunCase("manifest_missing_candidate", false, "manifest.tsv",
          [](const fs::path&, const fs::path& candidate) {
            return Remove(candidate / "manifest.tsv");
          });
  RunCase("manifest_mismatch", false, "differ",
          [](const fs::path&, const fs::path& candidate) {
            return WriteFile(candidate / "manifest.tsv", kBaseManifest + "extra\n");
           });
  RunCase("manifest_empty_candidate", false, "empty",
          [](const fs::path&, const fs::path& candidate) {
            return WriteFile(candidate / "manifest.tsv", "");
          });
  RunCase("manifest_no_rows", false, "no shader contract rows",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate, "\n");
          });
  RunCase("manifest_bad_columns", false, "4 tab-separated columns",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate, "main\tvs\tmain.vs.spv\n");
          });
  RunCase("bad_stage", false, "not vs or ps",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate,
                                 "main\tgs\tmain.vs.spv\tmain.hlsl\n");
          });
  RunCase("traversal_key", false, "safe local basename",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate,
                                 "..\tvs\tmain.vs.spv\tmain.hlsl\n");
          });
  RunCase("traversal_spv", false, "safe local basename",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate,
                                 "main\tvs\tsub/main.vs.spv\tmain.hlsl\n");
           });
  RunCase("windows_drive_spv", false, "safe local basename",
          [](const fs::path& legacy, const fs::path& candidate) {
            return ResetManifest(legacy, candidate, "main\tvs\tC:main.vs.spv\tmain.hlsl\n");
          });
  RunCase("missing_base_spv", false, "candidate SPIR-V main.vs.spv is missing",
          [](const fs::path&, const fs::path& candidate) {
            return Remove(candidate / "main.vs.spv");
          });
  RunCase("empty_base_spv", false, "candidate SPIR-V ui.ps.spv is empty",
          [](const fs::path&, const fs::path& candidate) {
            return WriteFile(candidate / "ui.ps.spv", "");
          });
  RunCase("empty_helper_spv_legacy", false, "legacy SPIR-V helper.vs.spv is empty",
          [](const fs::path& legacy, const fs::path&) {
            return WriteFile(legacy / "helper.vs.spv", "");
          });
  RunCase("missing_helper_spv", false, "candidate SPIR-V helper.vs.spv is missing",
          [](const fs::path&, const fs::path& candidate) {
            return Remove(candidate / "helper.vs.spv");
          });
  RunCase("empty_variant_spv", false, "candidate SPIR-V variant.ps.spv is empty",
          [](const fs::path&, const fs::path& candidate) {
            return WriteFile(candidate / "variant.ps.spv", "");
          });
  RunCase("missing_contract", false, "candidate HLSL contract main.vs.hlsl",
          [](const fs::path&, const fs::path& candidate) {
            return Remove(candidate / "main.vs.hlsl");
          });
  RunCase("contract_absent_both_allowed", true, "",
          [](const fs::path& legacy, const fs::path& candidate) {
            if (!ResetManifest(legacy, candidate,
                               "opt\tvs\topt.vs.spv\tnone\n")) {
              return false;
            }
            const char* const stale[] = {"main.vs.spv", "ui.ps.spv", "helper.vs.spv",
                                         "variant.ps.spv"};
            for (const char* name : stale) {
              if (!Remove(legacy / name) || !Remove(candidate / name)) return false;
            }
            return Remove(legacy / "main.vs.hlsl") && Remove(legacy / "ui.ps.hlsl") &&
                   Remove(candidate / "main.vs.hlsl") && Remove(candidate / "ui.ps.hlsl");
          });
  RunCase("no_spv_install_rejected", false, "no shader contract rows",
          [](const fs::path& legacy, const fs::path& candidate) {
            if (!ResetManifest(legacy, candidate, "\n")) return false;
            const char* const stale[] = {"main.vs.spv", "ui.ps.spv", "opt.vs.spv",
                                         "helper.vs.spv", "variant.ps.spv"};
            for (const char* name : stale) {
              if (!Remove(legacy / name) || !Remove(candidate / name)) return false;
            }
            return true;
          });

  std::cout << "nr_constants32_install_test: " << g_checks << " checks, " << g_failures
            << " failures\n";
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3) {
    const nr_constants32::Selection sel =
        nr_constants32::InspectCompleteSet(argv[1], argv[2]);
    if (sel.ok && sel.enabled && sel.error.empty()) {
      std::cout << "Inspector PASS: complete constants32 install\n";
      return 0;
    }
    std::cerr << "Inspector FAIL: "
              << (sel.error.empty() ? "incomplete install" : sel.error) << "\n";
    return 1;
  }
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [LEGACY CANDIDATE]\n";
    return 2;
  }
  const int code = RunTests();
  std::error_code ec;
  fs::remove_all(fs::path(kRoot), ec);
  if (ec) {
    std::cerr << "cannot remove fixture root: " << ec.message() << "\n";
    return code == 0 ? 1 : code;
  }
  return code;
}
