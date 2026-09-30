#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--record") {
        const fs::path output(argv[2]);
        fs::create_directories(output.parent_path());
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) return 3;
        for (int i = 3; i < argc; ++i) {
            if (i > 3) out << "\n";
            out << argv[i];
        }
        out.flush();
        return out ? 0 : 4;
    }

    if (argc >= 2) {
        const fs::path script(argv[1]);
        const fs::path marker(script.string() + ".submitted");
        std::ofstream out(marker, std::ios::binary | std::ios::trunc);
        if (!out) return 5;
        out << script.string() << "\n";
        out.flush();
        return out ? 0 : 6;
    }

    std::cerr << "usage: handler_fixture --record FILE [args...] | SCRIPT\n";
    return 2;
}
