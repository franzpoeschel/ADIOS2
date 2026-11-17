/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

#include "adios2/toolkit/sst/dp_config.h"

TEST(SstDataplaneConfig, ExplicitParametersBeatEnvironment)
{
    struct _SstParams Params = {0};
    ASSERT_EQ(SST_AddDataPlaneParam(&Params, "fabric_provider", "verbs"), 0);
    ASSERT_STREQ(get_dataplane_config(&Params, "FABRIC_PROVIDER"), "verbs");

#ifndef _WIN32
    setenv("FABRIC_PROVIDER", "shm", 1);
    EXPECT_STREQ(get_dataplane_config(&Params, "FABRIC_PROVIDER"), "verbs");
    unsetenv("FABRIC_PROVIDER");
#endif

    SST_FreeDataPlaneParams(&Params);
}

TEST(SstDataplaneConfig, EnvironmentFallback)
{
    struct _SstParams Params = {0};
#ifndef _WIN32
    setenv("FABRIC_IFACE", "ib0", 1);
    EXPECT_STREQ(get_dataplane_config(&Params, "FABRIC_IFACE"), "ib0");
    setenv("FABRIC_IFACE", "", 1);
    EXPECT_STREQ(get_dataplane_config(&Params, "fabric_iface"), "");
    unsetenv("FABRIC_IFACE");
#endif
    SST_FreeDataPlaneParams(&Params);
}

TEST(SstDataplaneConfig, InstanceOverridesWinAfterMerge)
{
    struct _SstParams Base = {0};
    struct _SstParams Override = {0};
    struct _SstParams Merged = {0};
    ASSERT_EQ(SST_AddDataPlaneParam(&Base, "FABRIC_PROVIDER", "verbs"), 0);
    ASSERT_EQ(SST_AddDataPlaneParam(&Base, "FABRIC_PROGRESS_THREAD", "1"), 0);
    ASSERT_EQ(SST_AddDataPlaneParam(&Override, "FABRIC_PROVIDER", "shm"), 0);

    SST_MergeDataPlaneParams(&Merged, &Base, &Override);
    EXPECT_STREQ(get_dataplane_config(&Merged, "FABRIC_PROVIDER"), "shm");
    EXPECT_STREQ(get_dataplane_config(&Merged, "FABRIC_PROGRESS_THREAD"), "1");

    SST_FreeDataPlaneParams(&Merged);
    SST_FreeDataPlaneParams(&Override);
    SST_FreeDataPlaneParams(&Base);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
