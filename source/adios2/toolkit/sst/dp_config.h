/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DP_CONFIG_H_
#define _DP_CONFIG_H_

#include "sst_data.h"

#ifdef __cplusplus
extern "C" {
#endif

int SST_AddDataPlaneParam(struct _SstParams *Params, const char *Key, const char *Value);
void SST_MergeDataPlaneParams(struct _SstParams *Out, const struct _SstParams *Base,
                              const struct _SstParams *Override);
void SST_FreeDataPlaneParams(struct _SstParams *Params);
const char *get_dataplane_config(const struct _SstParams *Params, const char *Key);

#ifdef __cplusplus
}
#endif

#endif /* _DP_CONFIG_H_ */
