/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#else
#define strcasecmp _stricmp
#define strdup _strdup
#endif

#include "dp_config.h"
#include "sst_data.h"

static char *Uppercase(const char *String)
{
    size_t Length = strlen(String);
    char *Result = malloc(Length + 1);
    for (size_t i = 0; i < Length; i++)
    {
        Result[i] = (char)toupper((unsigned char)String[i]);
    }
    Result[Length] = '\0';
    return Result;
}

int SST_AddDataPlaneParam(struct _SstParams *Params, const char *Key, const char *Value)
{
    char *UpperKey = Uppercase(Key);
    char *CopyValue = strdup(Value ? Value : "");

    char **NewKeys =
        realloc(Params->DataPlaneParamKeys, sizeof(char *) * (Params->DataPlaneParamCount + 1));
    if (!NewKeys)
    {
        free(UpperKey);
        free(CopyValue);
        return -1;
    }
    Params->DataPlaneParamKeys = NewKeys;

    char **NewValues =
        realloc(Params->DataPlaneParamValues, sizeof(char *) * (Params->DataPlaneParamCount + 1));
    if (!NewValues)
    {
        free(UpperKey);
        free(CopyValue);
        return -1;
    }
    Params->DataPlaneParamValues = NewValues;

    Params->DataPlaneParamKeys[Params->DataPlaneParamCount] = UpperKey;
    Params->DataPlaneParamValues[Params->DataPlaneParamCount] = CopyValue;
    Params->DataPlaneParamCount++;
    return 0;
}

void SST_FreeDataPlaneParams(struct _SstParams *Params)
{
    for (int i = 0; i < Params->DataPlaneParamCount; i++)
    {
        free(Params->DataPlaneParamKeys[i]);
        free(Params->DataPlaneParamValues[i]);
    }
    free(Params->DataPlaneParamKeys);
    free(Params->DataPlaneParamValues);
    Params->DataPlaneParamKeys = NULL;
    Params->DataPlaneParamValues = NULL;
    Params->DataPlaneParamCount = 0;
}

void SST_MergeDataPlaneParams(struct _SstParams *Out, const struct _SstParams *Base,
                              const struct _SstParams *Override)
{
    int BaseCount = Base ? Base->DataPlaneParamCount : 0;
    int OverrideCount = Override ? Override->DataPlaneParamCount : 0;
    int Total = BaseCount + OverrideCount;

    Out->DataPlaneParamCount = 0;
    Out->DataPlaneParamKeys = NULL;
    Out->DataPlaneParamValues = NULL;
    if (Total == 0)
    {
        return;
    }

    Out->DataPlaneParamKeys = calloc(Total, sizeof(char *));
    Out->DataPlaneParamValues = calloc(Total, sizeof(char *));
    for (int i = 0; i < OverrideCount; i++)
    {
        Out->DataPlaneParamKeys[i] = strdup(Override->DataPlaneParamKeys[i]);
        Out->DataPlaneParamValues[i] = strdup(Override->DataPlaneParamValues[i]);
    }
    for (int i = 0; i < BaseCount; i++)
    {
        int Index = OverrideCount + i;
        Out->DataPlaneParamKeys[Index] = strdup(Base->DataPlaneParamKeys[i]);
        Out->DataPlaneParamValues[Index] = strdup(Base->DataPlaneParamValues[i]);
    }
    Out->DataPlaneParamCount = Total;
}

const char *get_dataplane_config(const struct _SstParams *Params, const char *Key)
{
    if (Params)
    {
        for (int i = 0; i < Params->DataPlaneParamCount; i++)
        {
            if (strcasecmp(Params->DataPlaneParamKeys[i], Key) == 0)
            {
                return Params->DataPlaneParamValues[i];
            }
        }
    }

    char *UpperKey = Uppercase(Key);
    const char *Value = getenv(UpperKey);
    free(UpperKey);
    return Value;
}
