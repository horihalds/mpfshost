#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <unistd.h>

#include <cstdio>

int main(int argc, char* argv[])
{
    if (geteuid() == 0)
    {
        std::fputs("mpfshost tests must not run as root: they only cover unprivileged code.\n", stderr);
        return 1;
    }

    return Catch::Session().run(argc, argv);
}
