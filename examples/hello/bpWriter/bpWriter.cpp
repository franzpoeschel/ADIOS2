#include <adios2.h>
#include <string>
#include <vector>

#if ADIOS2_USE_MPI
#include <mpi.h>
#endif

int main(int argc, char *argv[])
{
#if ADIOS2_USE_MPI
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    adios2::ADIOS adios(MPI_COMM_WORLD);
#else
    adios2::ADIOS adios;
    int rank = 0;
#endif
    adios2::IO IO = adios.DeclareIO("parallel_write_zero_extent");

    // --- File creatIOn
    adios2::Engine writer = IO.Open("parallel_write_zero_extent.bp", adios2::Mode::Write);
    IO.DefineAttribute<unsigned char>("__openPMD_internal/useModifiableAttributes", 1);

    std::vector<double> pos_x = {42.0};
    std::vector<unsigned long long> posOff_x = {42};

    adios2::Variable<double> position_x;
    adios2::Variable<unsigned long long> positionOffset_x;

    for (size_t i = 0; i < 10; ++i)
    {
        writer.BeginStep();
        IO.DefineAttribute("__openPMD_groups/data", (unsigned long long)0, "", "/", true);
        IO.DefineAttribute("__openPMD_groups/", (unsigned long long)0, "", "/", true);
        IO.DefineAttribute("__openPMD_groups/data", (unsigned long long)0, "", "/", true);
        if (i == 0)
        {
            IO.DefineAttribute("__openPMD_internal/openPMD2_adios2_schema", (unsigned long long)0);
            writer.PerformDataWrite();
            IO.DefineAttribute<std::string>("/particlesPath", "particles/", "", "/", true);
        }
        if (rank > 0)
        {
            IO.DefineAttribute("__openPMD_groups/data/particles", (unsigned long long)0, "", "/",
                               true);
            IO.DefineAttribute("__openPMD_groups/data", (unsigned long long)0, "", "/", true);
            IO.DefineAttribute("__openPMD_groups/data", (unsigned long long)0, "", "/", true);
            IO.DefineAttribute("__openPMD_groups/", (unsigned long long)0, "", "/", true);
            IO.DefineAttribute("__openPMD_groups/data/particles/e", (unsigned long long)0, "", "/",
                               true);
            IO.DefineAttribute("__openPMD_groups/data/particles/e/position", (unsigned long long)0,
                               "", "/", true);
            position_x = i == 0 ? IO.DefineVariable<double>("/data/particles/e/position/x", {1}, {0}, {1})
                                : IO.InquireVariable<double>("/data/particles/e/position/x");
            IO.DefineAttribute("/data/particles/e/position/x/unitSI", (double)1, "", "/", true);
            IO.DefineAttribute("/data/particles/e/position/timeOffset", (float)0, "", "/", true);
            std::vector<double> unitDimension1{1, 0, 0, 0, 0, 0, 0};
            IO.DefineAttribute("/data/particles/e/position/unitDimension", unitDimension1.data(),
                               unitDimension1.size(), "", "/", true);
            IO.DefineAttribute("__openPMD_groups/data/particles/e/positionOffset",
                               (unsigned long long)0, "", "/", true);
            positionOffset_x =
                i == 0
                    ? IO.DefineVariable<unsigned long long>("/data/particles/e/positionOffset/x",
                                                            {1}, {0}, {1})
                    : IO.InquireVariable<unsigned long long>("/data/particles/e/positionOffset/x");
            IO.DefineAttribute("/data/particles/e/positionOffset/x/unitSI", (double)1, "", "/",
                               true);
            IO.DefineAttribute("/data/particles/e/positionOffset/timeOffset", (float)0, "", "/",
                               true);
            std::vector<double> unitDimension2{1, 0, 0, 0, 0, 0, 0};
            IO.DefineAttribute("/data/particles/e/positionOffset/unitDimension",
                               unitDimension2.data(), unitDimension2.size(), "", "/", true);
        }
        IO.DefineAttribute("/data/dt", (double)1, "", "/", true);
        IO.DefineAttribute("/data/time", (double)0, "", "/", true);
        IO.DefineAttribute("/data/timeUnitSI", (double)1, "", "/", true);
        IO.DefineAttribute<std::string>("/data/yolo", "yo", "", "/", true);
        if (i == 0)
        {
            IO.DefineAttribute<std::string>("/basePath", "/data/%T/", "", "/", true);
            IO.DefineAttribute<std::string>("/date", "2025-11-11 14:56:08 +0100", "", "/", true);
            IO.DefineAttribute<std::string>("/iterationEncoding", "variableBased", "", "/", true);
            IO.DefineAttribute<std::string>("/iterationFormat", "/data", "", "/", true);
            IO.DefineAttribute<std::string>("/openPMD", "1.1.0", "", "/", true);
            IO.DefineAttribute("/openPMDextension", (unsigned int)0, "", "/", true);
            IO.DefineAttribute<std::string>("/particlesPath", "particles/", "", "/", true);
            IO.DefineAttribute<std::string>("/software", "openPMD-api", "", "/", true);
            IO.DefineAttribute<std::string>("/softwareVersion", "0.17.0-dev", "", "/", true);
        }
        if(rank > 0)
        {
            // offs./ext.: [0] / [1]
            // var = /data/particles/e/position/x
            writer.Put(position_x, pos_x.data());
            // offs./ext.: [0] / [1]
            // var = /data/particles/e/positionOffset/x
            writer.Put(positionOffset_x, posOff_x.data());
        }
        writer.PerformDataWrite();
        IO.DefineAttribute("/data/dt", (double)1, "", "/", true);
        IO.DefineAttribute("/data/time", (double)0, "", "/", true);
        IO.DefineAttribute("/data/timeUnitSI", (double)1, "", "/", true);
        IO.DefineAttribute<std::string>("/data/yolo", "yo", "", "/", true);
        std::vector<unsigned long long> snapshot{0};
        IO.DefineAttribute("/data/snapshot", snapshot.data(), snapshot.size(), "", "/", true);
        IO.DefineAttribute("__openPMD_internal/useSteps", (unsigned char)1);
        writer.EndStep();

        IO.DefineAttribute("__openPMD_groups/data", (unsigned long long)0, "", "/", true);
    }

    writer.Close();
#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif
    return 0;
}

#if ADIOS2_USE_MPI
int main2(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO IO = adios.DeclareIO("parallel_write_zero_extent");

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // --- File creatIOn
    adios2::Engine writer = IO.Open("parallel_write_zero_extent.bp", adios2::Mode::Write);
    IO.DefineAttribute<unsigned char>("__openPMD_internal/useModifiableAttributes", 1);

    for (size_t i = 0; i < 10; ++i)
    {
        // --- Begin Step
        writer.BeginStep();
        IO.DefineAttribute<double>("/data/dt", 0.1, "", "/", true);
        IO.DefineAttribute<double>("/data/time", 1.23, "", "/", true);
        IO.DefineAttribute<double>("/data/timeUnitSI", 1.0, "", "/", true);

        if (i == 0)
        {
            // --- Top-level metadata
            IO.DefineAttribute<std::string>("particlesPath", "particles/", "", "/", true);
            IO.DefineAttribute<std::string>("basePath", "/data/", "", "/", true);
            IO.DefineAttribute<std::string>("date", "2025-11-11", "", "/", true);
            IO.DefineAttribute<std::string>("iteratIOnEncoding", "groupBased", "", "/", true);
            IO.DefineAttribute<std::string>("iteratIOnFormat", "%T", "", "/", true);
            IO.DefineAttribute<std::string>("openPMD", "1.1.0", "", "/", true);
            IO.DefineAttribute<unsigned int>("openPMDextensIOn", 1, "", "/", true);
            IO.DefineAttribute<std::string>("software", "ADIOS2-test", "", "/", true);
            IO.DefineAttribute<std::string>("softwareVersIOn", "1.0", "", "/", true);
        }

        IO.DefineAttribute<std::string>("/data/yolo", "yes", "", "/", true);

        if (rank > 0)
        {
            auto varX =
                i == 0 ? IO.DefineVariable<double>("/data/particles/e/positIOn/x", {1}, {0}, {1})
                       : IO.InquireVariable<double>("/data/particles/e/positIOn/x");
            // --- Example variable creatIOn (extent = [1])
            std::vector<double> x = {42.0};

            writer.Put(varX, x.data());

            // --- Example attributes

            IO.DefineAttribute<double>("/data/particles/e/positIOn/unitSI", 1.0, "", "/", true);
            IO.DefineAttribute<float>("/data/particles/e/positIOn/timeOffset", 0.0f, "", "/", true);
            std::vector<double> dims = {1, 0, 0, 0, 0, 0, 0};
            IO.DefineAttribute<double>("/data/particles/e/positIOn/unitDimensIOn", dims.data(),
                                       dims.size(), "", "/", true);

            // --- Write data
            writer.Put(varX, x.data());
        }
        std::vector<unsigned long long> snapshot = {static_cast<unsigned long long>(i)};
        IO.DefineAttribute<unsigned long long>("snapshot", snapshot.data(), snapshot.size(), "",
                                               "/", true);

        writer.EndStep();
    }

    writer.Close();
    MPI_Finalize();
    return 0;
}
#endif