// Replays fuzz inputs without libFuzzer, so every toolchain can build the fuzz targets and run
// the seed corpus and saved crash reproducers as regression tests.
//
//     pace_fuzz_http1_server tests/fuzz/corpus/http1_server crash-1234
//
// Each argument is a file or a directory of files; arguments starting with '-' are ignored so the
// same command lines work with libFuzzer builds.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace
{
void replay(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}
} // namespace

int main(int argc, char** argv)
{
    std::size_t replayed = 0;
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument.starts_with('-'))
            {
                continue;
            }
            const std::filesystem::path path(argument);
            if (std::filesystem::is_directory(path))
            {
                for (const auto& entry : std::filesystem::directory_iterator(path))
                {
                    if (entry.is_regular_file())
                    {
                        replay(entry.path());
                        ++replayed;
                    }
                }
            }
            else
            {
                replay(path);
                ++replayed;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "replay failed: %s\n", error.what());
        return 1;
    }
    if (replayed == 0)
    {
        std::fprintf(stderr, "usage: %s <input file or corpus directory>...\n", argv[0]);
        return 2;
    }
    std::printf("replayed %zu inputs\n", replayed);
    return 0;
}
