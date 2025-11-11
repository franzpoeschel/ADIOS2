#if false || ADIOS2_USE_MPI
#include <adios2.h>
#include <string>
#include <vector>

#include <mpi.h>

int main(int argc, char *argv[])
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
#else
int main() {}
#endif