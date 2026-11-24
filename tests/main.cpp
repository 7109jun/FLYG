// flyG - test runner
// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>

#include "test_framework.hpp"

int flyg_child_main(int argc, char** argv);  // helper process used by the process tests

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return flyg_child_main(argc, argv);
    return flyg_test::run_all(argc > 1 ? argv[1] : nullptr);
}
