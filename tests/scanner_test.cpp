#include <catch2/catch.hpp>

#include "core/target/scanner.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
    class ScannerFixture
    {
    public:
        ScannerFixture()
        {
            std::filesystem::create_directories(MPFSHOST_TEST_TMP_DIR);
            std::string pattern = (std::filesystem::path {MPFSHOST_TEST_TMP_DIR} / "scanner-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            _root   = pattern;
            _pidDir = _root / "qemu";
            _qmpDir = _root / "qmp";
            std::filesystem::create_directories(_pidDir);
            std::filesystem::create_directories(_qmpDir);
        }

        ~ScannerFixture()
        {
            std::error_code error;
            std::filesystem::remove_all(_root, error);
        }

        ScannerFixture(const ScannerFixture&)            = delete;
        ScannerFixture& operator=(const ScannerFixture&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return _root;
        }

        [[nodiscard]] const std::filesystem::path& pidDir() const noexcept
        {
            return _pidDir;
        }

        [[nodiscard]] const std::filesystem::path& qmpDir() const noexcept
        {
            return _qmpDir;
        }

        void addPidFile(const std::string& name, const std::string& content) const
        {
            writeFile(_pidDir / (name + ".pid"), content);
        }

        void addSocket(const std::string& name) const
        {
            writeFile(_qmpDir / (name + ".sock"), "");
        }

    private:
        static void writeFile(const std::filesystem::path& path, const std::string& content)
        {
            std::ofstream file {path};
            REQUIRE(file);
            file << content;
        }

        std::filesystem::path _root;
        std::filesystem::path _pidDir;
        std::filesystem::path _qmpDir;
    };
} // namespace

TEST_CASE("scanner pairs a pid file with its qmp socket", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("vm-alpha", "1234\n");
    fixture.addSocket("vm-alpha");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    const auto                    targets = scanner.scan();

    REQUIRE(targets.size() == 1);
    REQUIRE(targets.front().name == "vm-alpha");
    REQUIRE(targets.front().qemuPid == 1234);
    REQUIRE(targets.front().qmpSocket == fixture.qmpDir() / "vm-alpha.sock");
}

TEST_CASE("scanner ignores a pid file without a socket", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("vm-alpha", "10");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    REQUIRE(scanner.scan().empty());
}

TEST_CASE("scanner ignores a socket without a pid file", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addSocket("vm-alpha");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    REQUIRE(scanner.scan().empty());
}

TEST_CASE("scanner skips malformed pid files", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("garbage", "not-a-number");
    fixture.addSocket("garbage");
    fixture.addPidFile("empty", "");
    fixture.addSocket("empty");
    fixture.addPidFile("negative", "-5");
    fixture.addSocket("negative");
    fixture.addPidFile("trailing", "42 is the answer");
    fixture.addSocket("trailing");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    REQUIRE(scanner.scan().empty());
}

TEST_CASE("scanner accepts non-numeric vm names and dotted names", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("ubuntu-22.04", "77\n");
    fixture.addSocket("ubuntu-22.04");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    const auto                    targets = scanner.scan();

    REQUIRE(targets.size() == 1);
    REQUIRE(targets.front().name == "ubuntu-22.04");
}

TEST_CASE("scanner ignores files without the pid extension", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("vm-alpha", "10");
    fixture.addSocket("vm-alpha");

    std::ofstream other {fixture.pidDir() / "notes.txt"};
    REQUIRE(other);
    other << "10";

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    const auto                    targets = scanner.scan();

    REQUIRE(targets.size() == 1);
    REQUIRE(targets.front().name == "vm-alpha");
}

TEST_CASE("scanner sorts targets by name", "[scanner]")
{
    ScannerFixture fixture;
    fixture.addPidFile("zeta", "30");
    fixture.addSocket("zeta");
    fixture.addPidFile("alpha", "10");
    fixture.addSocket("alpha");
    fixture.addPidFile("middle", "20");
    fixture.addSocket("middle");

    const mpfshost::TargetScanner scanner {fixture.pidDir(), fixture.qmpDir()};
    const auto                    targets = scanner.scan();

    REQUIRE(targets.size() == 3);
    REQUIRE(targets[0].name == "alpha");
    REQUIRE(targets[1].name == "middle");
    REQUIRE(targets[2].name == "zeta");
}

TEST_CASE("scanner returns an empty list when its roots do not exist", "[scanner]")
{
    ScannerFixture fixture;

    const mpfshost::TargetScanner scanner {fixture.root() / "missing-pids", fixture.root() / "missing-qmp"};
    REQUIRE(scanner.scan().empty());
}
