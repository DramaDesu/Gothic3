// Command line front end for the archive reader: list or extract entries.

#include "genome/pak.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

void usage()
{
    std::puts("usage:\n"
              "  g3pak list    <archive.pak> [substring]\n"
              "  g3pak extract <archive.pak> <name> [output]");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        usage();
        return 2;
    }

    const std::string command = argv[1];
    std::string error;
    const auto archive = genome::PakArchive::open(argv[2], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (command == "list")
    {
        const std::string filter = argc > 3 ? argv[3] : "";
        std::size_t shown = 0;
        for (const genome::PakEntry &entry : archive->entries())
        {
            if (entry.deleted)
                continue;
            if (!filter.empty() && entry.path.find(filter) == std::string::npos)
                continue;
            std::printf("%12llu  %s%s\n", static_cast<unsigned long long>(entry.size), entry.path.c_str(),
                        entry.compressed() ? "" : "  (stored)");
            ++shown;
        }
        std::printf("-- %zu of %zu entries\n", shown, archive->entries().size());
        return 0;
    }

    if (command == "extract" && argc >= 4)
    {
        const std::vector<std::uint8_t> bytes = archive->read(argv[3], &error);
        if (bytes.empty())
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        const std::string output = argc > 4 ? argv[4] : std::string(argv[3]);
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::printf("wrote %zu bytes to %s\n", bytes.size(), output.c_str());
        return 0;
    }

    usage();
    return 2;
}
