#if true || ADIOS2_USE_MPI
#include <adios2.h>
#include <string>
#include <vector>

#include <mpi.h>

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO io = adios.DeclareIO("parallel_write_zero_extent");

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // --- File creation
    adios2::Engine writer = io.Open("parallel_write_zero_extent.bp", adios2::Mode::Write);
    io.DefineAttribute<unsigned char>("__openPMD_internal/useModifiableAttributes", 1);

    for (size_t i = 0; i < 10; ++i)
    {
        // --- Begin Step
        writer.BeginStep();
        io.DefineAttribute<double>("/data/dt", 0.1, "", "/", true);
        io.DefineAttribute<double>("/data/time", 1.23, "", "/", true);
        io.DefineAttribute<double>("/data/timeUnitSI", 1.0, "", "/", true);

        if (i == 0)
        {
            // --- Top-level metadata
            io.DefineAttribute<std::string>("particlesPath", "particles/", "", "/", true);
            io.DefineAttribute<std::string>("basePath", "/data/", "", "/", true);
            io.DefineAttribute<std::string>("date", "2025-11-11", "", "/", true);
            io.DefineAttribute<std::string>("iterationEncoding", "groupBased", "", "/", true);
            io.DefineAttribute<std::string>("iterationFormat", "%T", "", "/", true);
            io.DefineAttribute<std::string>("openPMD", "1.1.0", "", "/", true);
            io.DefineAttribute<unsigned int>("openPMDextension", 1, "", "/", true);
            io.DefineAttribute<std::string>("software", "ADIOS2-test", "", "/", true);
            io.DefineAttribute<std::string>("softwareVersion", "1.0", "", "/", true);
        }

        io.DefineAttribute<std::string>("/data/yolo", "yes", "", "/", true);

        if (rank > 0)
        {
            auto varX =
                i == 0 ? io.DefineVariable<double>("/data/particles/e/position/x", {1}, {0}, {1})
                       : io.InquireVariable<double>("/data/particles/e/position/x");
            // --- Example variable creation (extent = [1])
            std::vector<double> x = {42.0};

            writer.Put(varX, x.data());

            // --- Example attributes

            io.DefineAttribute<double>("/data/particles/e/position/unitSI", 1.0, "", "/", true);
            io.DefineAttribute<float>("/data/particles/e/position/timeOffset", 0.0f, "", "/", true);
            std::vector<double> dims = {1, 0, 0, 0, 0, 0, 0};
            io.DefineAttribute<double>("/data/particles/e/position/unitDimension", dims.data(),
                                       dims.size(), "", "/", true);

            // --- Write data
            writer.Put(varX, x.data());
        }
        std::vector<unsigned long long> snapshot = {static_cast<unsigned long long>(i)};
        io.DefineAttribute<unsigned long long>("snapshot", snapshot.data(), snapshot.size(), "",
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