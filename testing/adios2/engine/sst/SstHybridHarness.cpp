/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <mpi.h>

#include <adios2/common/ADIOSTypes.h>
#include <adios2/cxx/ADIOS.h>
#include <adios2/cxx/Engine.h>
#include <adios2/cxx/IO.h>
#include <adios2/cxx/Variable.h>

namespace
{
constexpr double WriterRankStride = 1000000.0;
constexpr double StepStride = 1000.0;
constexpr float StepTimeoutSeconds = 120.f;
constexpr size_t HostnameSize = 256;

struct Options
{
    std::string Mode = "split";
    std::string Filename = "sst_hybrid_harness";
    std::string Config;
    std::string DataPlane = "hybrid";
    size_t Steps = 5;
    size_t Elements = 1024;
    int WriterRanks = -1;
};
} // namespace

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto Usage(const char *Program) -> void
{
    std::cerr << "Usage: " << Program << " [options]\n"
              << "  --mode split|writer|reader   default: split\n"
              << "  --filename NAME              default: sst_hybrid_harness\n"
              << "  --config STRING              set SST_HYBRID_DP via IO.SetParameter\n"
              << "  --dataplane NAME             default: hybrid\n"
              << "  --steps N                    default: 5\n"
              << "  --elements N                 default: 1024\n"
              << "  --writers N                  required for reader mode\n";
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto ParseOptions(int argc, char **argv, Options &Options)
    -> bool // NOLINT(misc-use-anonymous-namespace)
{
    for (int i = 1; i < argc; i++)
    {
        const std::string Argument =
            argv[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        auto Next = [&](const char *Name) -> std::string {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << Name << "\n";
                std::exit(2);
            }
            return argv[++i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        };
        if (Argument == "--mode")
        {
            Options.Mode = Next("--mode");
        }
        else if (Argument == "--filename")
        {
            Options.Filename = Next("--filename");
        }
        else if (Argument == "--config")
        {
            Options.Config = Next("--config");
        }
        else if (Argument == "--dataplane")
        {
            Options.DataPlane = Next("--dataplane");
        }
        else if (Argument == "--steps")
        {
            Options.Steps = std::stoul(Next("--steps"));
        }
        else if (Argument == "--elements")
        {
            Options.Elements = std::stoul(Next("--elements"));
        }
        else if (Argument == "--writers")
        {
            Options.WriterRanks = std::stoi(Next("--writers"));
        }
        else if (Argument == "--help" || Argument == "-h")
        {
            Usage(argv[0]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::exit(0);
        }
        else
        {
            std::cerr << "Unknown argument: " << Argument << "\n";
            Usage(argv[0]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto ConfigureIO(adios2::IO &IO, const Options &Options)
    -> void // NOLINT(misc-use-anonymous-namespace)
{
    IO.SetEngine("SST");
    IO.SetParameter("OpenTimeoutSecs", "120");
    IO.SetParameter("SST_REQUIRE_DATAPLANE", Options.DataPlane);
    if (!Options.Config.empty())
    {
        IO.SetParameter("SST_HYBRID_DP", Options.Config);
    }
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto ExpectedValue(int WriterRank, size_t Step, size_t Element)
    -> double // NOLINT(misc-use-anonymous-namespace)
{
    return static_cast<double>(WriterRank) * WriterRankStride +
           static_cast<double>(Step) * StepStride + static_cast<double>(Element);
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto RunWriter(MPI_Comm Comm, const Options &Options)
    -> int // NOLINT(misc-use-anonymous-namespace)
{
    int Rank = 0;
    int Size = 1;
    MPI_Comm_rank(Comm, &Rank);
    MPI_Comm_size(Comm, &Size);

    adios2::ADIOS Adios(Comm);
    adios2::IO IO = Adios.DeclareIO("SSTHybridHarnessWriter");
    ConfigureIO(IO, Options);
    const adios2::Variable<double> Variable = IO.DefineVariable<double>(
        "data", {static_cast<size_t>(Size) * Options.Elements},
        {static_cast<size_t>(Rank) * Options.Elements}, {Options.Elements});

    adios2::Engine Engine = IO.Open(Options.Filename, adios2::Mode::Write);
    std::vector<double> Local(Options.Elements);
    for (size_t Step = 0; Step < Options.Steps; Step++)
    {
        for (size_t i = 0; i < Options.Elements; i++)
        {
            Local[i] = ExpectedValue(Rank, Step, i);
        }
        Engine.BeginStep();
        Engine.Put<double>(Variable, Local.data());
        Engine.EndStep();
    }
    Engine.Close();
    return 0;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto RunReader(MPI_Comm Comm, const Options &Options, int WriterRanks)
    -> int // NOLINT(misc-use-anonymous-namespace)
{
    int Rank = 0;
    MPI_Comm_rank(Comm, &Rank);

    adios2::ADIOS Adios(Comm);
    adios2::IO IO = Adios.DeclareIO("SSTHybridHarnessReader");
    ConfigureIO(IO, Options);
    adios2::Engine Engine = IO.Open(Options.Filename, adios2::Mode::Read);

    size_t Steps = 0;
    int Failures = 0;
    while (Engine.BeginStep(adios2::StepMode::Read, StepTimeoutSeconds) == adios2::StepStatus::OK)
    {
        adios2::Variable<double> Variable = IO.InquireVariable<double>("data");
        if (!Variable)
        {
            std::cerr << "Reader rank " << Rank << ": missing variable data\n";
            Failures++;
            Engine.EndStep();
            break;
        }

        std::vector<double> Local(Options.Elements);
        for (int WriterRank = 0; WriterRank < WriterRanks; WriterRank++)
        {
            Variable.SetSelection(
                {{static_cast<size_t>(WriterRank) * Options.Elements}, {Options.Elements}});
            Engine.Get<double>(Variable, Local, adios2::Mode::Sync);
            for (size_t i = 0; i < Options.Elements; i++)
            {
                const double Expected = ExpectedValue(WriterRank, Steps, i);
                if (Local[i] != Expected)
                {
                    std::cerr << "Reader rank " << Rank << ", step " << Steps << ", writer rank "
                              << WriterRank << ", element " << i << ": got " << Local[i]
                              << ", expected " << Expected << "\n";
                    Failures++;
                    break;
                }
            }
        }
        Engine.EndStep();
        Steps++;
    }
    Engine.Close();

    if (Steps == 0)
    {
        std::cerr << "Reader rank " << Rank << ": no steps received\n";
        Failures++;
    }
    if (Rank == 0)
    {
        std::cout << "reader finished " << Steps << " step(s), failures=" << Failures << "\n";
    }
    return Failures == 0 ? 0 : 1;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static auto RunSplit(MPI_Comm World, const Options &Options)
    -> int // NOLINT(misc-use-anonymous-namespace)
{
    int WorldRank = 0;
    int WorldSize = 1;
    MPI_Comm_rank(World, &WorldRank);
    MPI_Comm_size(World, &WorldSize);

    std::array<char, HostnameSize> Hostname = {0};
#ifndef _WIN32
    gethostname(Hostname.data(), Hostname.size() - 1);
#endif

    const int Color = WorldRank % 2;
    MPI_Comm RoleComm = MPI_COMM_NULL;
    MPI_Comm_split(World, Color, WorldRank, &RoleComm);

    const int WriterRanks = static_cast<int>((WorldSize + 1) / 2);
    if (WorldRank == 0)
    {
        std::cout << "split job: " << WorldSize << " ranks, " << WriterRanks
                  << " writer rank(s), host " << Hostname.data() << "\n";
    }

    int Result = 0;
    if (Color == 0)
    {
        Result = RunWriter(RoleComm, Options);
    }
    else
    {
        Result = RunReader(RoleComm, Options, WriterRanks);
    }

    MPI_Comm_free(&RoleComm);

    int GlobalResult = 0;
    MPI_Allreduce(&Result, &GlobalResult, 1, MPI_INT, MPI_MAX, World);
    return GlobalResult;
}

auto main(int argc, char **argv) -> int
{
    MPI_Init(&argc, &argv);

    Options Parsed;
    if (!ParseOptions(argc, argv, Parsed))
    {
        MPI_Finalize();
        return 2;
    }

    int Rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &Rank);

    int Result = 0;
    if (Parsed.Mode == "split")
    {
        Result = RunSplit(MPI_COMM_WORLD, Parsed);
    }
    else if (Parsed.Mode == "writer")
    {
        Result = RunWriter(MPI_COMM_WORLD, Parsed);
    }
    else if (Parsed.Mode == "reader")
    {
        if (Parsed.WriterRanks <= 0)
        {
            if (Rank == 0)
            {
                std::cerr << "--writers is required in reader mode\n";
            }
            MPI_Finalize();
            return 2;
        }
        Result = RunReader(MPI_COMM_WORLD, Parsed, Parsed.WriterRanks);
    }
    else
    {
        if (Rank == 0)
        {
            std::cerr << "Unknown mode: " << Parsed.Mode << "\n";
        }
        MPI_Finalize();
        return 2;
    }

    MPI_Finalize();
    return Result;
}
