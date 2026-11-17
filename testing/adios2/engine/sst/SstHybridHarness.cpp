/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <mpi.h>

#include <adios2.h>

namespace
{
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

void Usage(const char *Program)
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

bool ParseOptions(int argc, char **argv, Options &Options)
{
    for (int i = 1; i < argc; i++)
    {
        std::string Argument = argv[i];
        auto Next = [&](const char *Name) -> std::string {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << Name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (Argument == "--mode")
            Options.Mode = Next("--mode");
        else if (Argument == "--filename")
            Options.Filename = Next("--filename");
        else if (Argument == "--config")
            Options.Config = Next("--config");
        else if (Argument == "--dataplane")
            Options.DataPlane = Next("--dataplane");
        else if (Argument == "--steps")
            Options.Steps = std::stoul(Next("--steps"));
        else if (Argument == "--elements")
            Options.Elements = std::stoul(Next("--elements"));
        else if (Argument == "--writers")
            Options.WriterRanks = std::stoi(Next("--writers"));
        else if (Argument == "--help" || Argument == "-h")
        {
            Usage(argv[0]);
            std::exit(0);
        }
        else
        {
            std::cerr << "Unknown argument: " << Argument << "\n";
            Usage(argv[0]);
            return false;
        }
    }
    return true;
}

void ConfigureIO(adios2::IO &IO, const Options &Options)
{
    IO.SetEngine("SST");
    IO.SetParameter("OpenTimeoutSecs", "120");
    IO.SetParameter("SST_REQUIRE_DATAPLANE", Options.DataPlane);
    if (!Options.Config.empty())
    {
        IO.SetParameter("SST_HYBRID_DP", Options.Config);
    }
}

double ExpectedValue(int WriterRank, size_t Step, size_t Element)
{
    return static_cast<double>(WriterRank) * 1000000.0 + static_cast<double>(Step) * 1000.0 +
           static_cast<double>(Element);
}

int RunWriter(MPI_Comm Comm, const Options &Options)
{
    int Rank = 0;
    int Size = 1;
    MPI_Comm_rank(Comm, &Rank);
    MPI_Comm_size(Comm, &Size);

    adios2::ADIOS Adios(Comm);
    adios2::IO IO = Adios.DeclareIO("SSTHybridHarnessWriter");
    ConfigureIO(IO, Options);
    adios2::Variable<double> Variable = IO.DefineVariable<double>(
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

int RunReader(MPI_Comm Comm, const Options &Options, int WriterRanks)
{
    int Rank = 0;
    MPI_Comm_rank(Comm, &Rank);

    adios2::ADIOS Adios(Comm);
    adios2::IO IO = Adios.DeclareIO("SSTHybridHarnessReader");
    ConfigureIO(IO, Options);
    adios2::Engine Engine = IO.Open(Options.Filename, adios2::Mode::Read);

    size_t Steps = 0;
    int Failures = 0;
    while (Engine.BeginStep(adios2::StepMode::Read, 120.f) == adios2::StepStatus::OK)
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
                double Expected = ExpectedValue(WriterRank, Steps, i);
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

int RunSplit(MPI_Comm World, const Options &Options)
{
    int WorldRank = 0;
    int WorldSize = 1;
    MPI_Comm_rank(World, &WorldRank);
    MPI_Comm_size(World, &WorldSize);

    char Hostname[256] = {0};
#ifndef _WIN32
    gethostname(Hostname, sizeof(Hostname) - 1);
#endif

    int Color = WorldRank % 2;
    MPI_Comm RoleComm = MPI_COMM_NULL;
    MPI_Comm_split(World, Color, WorldRank, &RoleComm);

    int RoleSize = 0;
    MPI_Comm_size(RoleComm, &RoleSize);
    int WriterRanks = static_cast<int>((WorldSize + 1) / 2);
    if (WorldRank == 0)
    {
        std::cout << "split job: " << WorldSize << " ranks, " << WriterRanks
                  << " writer rank(s), host " << Hostname << "\n";
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
} // namespace

int main(int argc, char **argv)
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
