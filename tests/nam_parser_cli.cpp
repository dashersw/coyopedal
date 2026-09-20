#include "nam_json.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(input)), {});
    std::vector<uint8_t> data(48616);
    char error[128]{};
    if (!pedalboard_parse_nam(json.data(), json.size(), data.data(), data.size(), error,
                              sizeof error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), data.size());
}
