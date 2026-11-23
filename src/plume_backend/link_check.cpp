#include "backend.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
int selection_check() {
    using wwe2k14::plume_backend::DeviceSelectionError;
    using wwe2k14::plume_backend::select_unique_device_name;

    if (select_unique_device_name({}, "GPU").error != DeviceSelectionError::name_not_found ||
        select_unique_device_name({"GPU"}, "").error != DeviceSelectionError::empty_preferred_name ||
        select_unique_device_name({"GPU", "GPU"}, "GPU").error != DeviceSelectionError::duplicate_name ||
        select_unique_device_name({"CPU", "GPU"}, "GPU").exact_name != "GPU") {
        std::cerr << "device-name selection check failed\n";
        return 1;
    }
    std::cout << "device-name selection check passed\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--selection-check") {
        return selection_check();
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
        // Taking the method address forces the backend object and its pinned
        // Plume references through the static link. This path never calls it.
        using Initialize = bool (wwe2k14::plume_backend::Backend::*)(
            const std::string&, std::string*);
        volatile Initialize linked_symbol = &wwe2k14::plume_backend::Backend::initialize;
        (void)linked_symbol;
        std::cout << "usage: plume_backend_link_check --help | --selection-check\n";
        return 0;
    }
    std::cerr << "This CPU-only link check accepts --help or --selection-check.\n";
    return 2;
}
