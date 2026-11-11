/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#include "adaptive_resolution_cases.hpp"
#include "output_framerate_cases.hpp"

int main() {
  bool passed = adaptive_resolution_cases::Run();
  RunOutputFramerateCases([&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "output-framerate: " << name << '\n';
      passed = false;
    }
  });
  return passed ? 0 : 1;
}
