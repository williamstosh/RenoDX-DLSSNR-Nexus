#include <iostream>
#include <string_view>

#include <nr_model.hpp>

int main() {
  using renodx::addons::dlss5::nr_model::SupportsControlMask;
  struct Case {
    std::string_view version;
    bool supported;
  };
  constexpr Case cases[] = {
      {"310.9.16.0", true},
      {"310.9.16.1", false},
      {"310.9.15.0", false},
      {"310.8.0.0", false},
      {"", false},
      {"unknown", false},
      {"310.9.16", false},
      {"0310.9.16.0", false},
      {"310.9.16.0 ", false},
  };
  for (const auto& test : cases) {
    if (SupportsControlMask(test.version) != test.supported) {
      std::cerr << "FAIL: SupportsControlMask(\"" << test.version
                << "\") expected " << test.supported << '\n';
      return 1;
    }
  }
  std::cout << (sizeof(cases) / sizeof(cases[0]))
            << " checks, 0 failures\n";
  return 0;
}
