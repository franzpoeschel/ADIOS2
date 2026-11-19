/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <atl.h>
#include <evpath.h>
#include <ffs.h>

#include "SSTConfig.h"
#include "dp_config.h"
#include "dp_interface.h"

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strdup _strdup
#endif

#define HYBRID_MAX_SUB_DPS 8

enum HybridSubDPScope
{
    HybridScopeAny = 0,
    HybridScopeLocal,
    HybridScopeRemote
};

typedef struct _HybridSubDPConfig
{
    CP_DP_Interface Interface;
    char *TypeName;
    char *Name;
    enum HybridSubDPScope Scope;
    struct _SstParams Params;
} *HybridSubDPConfig;

typedef struct _HybridConfig
{
    int NumSubDPs;
    struct _HybridSubDPConfig SubDPs[HYBRID_MAX_SUB_DPS];
    FFSContext FFS;
    FMFormat ReaderContactFormats[HYBRID_MAX_SUB_DPS];
    FMFormat WriterContactFormats[HYBRID_MAX_SUB_DPS];
    FMFormat TimestepInfoFormats[HYBRID_MAX_SUB_DPS];
} *HybridConfig;

typedef struct _HybridSubDPInfo
{
    char *Name;
    int Length;
    int Available;
    char *Address;
} *HybridSubDPInfo;

typedef struct _HybridContactInfo
{
    int NumSubDPs;
    char *Hostname;
    struct _HybridSubDPInfo *SubInfo;
} *HybridContactInfo;

typedef struct _HybridTimestepInfo
{
    int NumSubInfos;
    struct _HybridSubDPInfo *SubInfo;
} *HybridTimestepInfo;

typedef struct _HybridSubDPState
{
    CP_DP_Interface Interface;
    const char *Name;
    enum HybridSubDPScope Scope;
    DP_RS_Stream RS_Stream;
    DP_WS_Stream WS_Stream;
    DP_WSR_Stream WSR_Stream;
    int Ready;
} *HybridSubDPState;

typedef struct _HybridTimestepEntry
{
    size_t Timestep;
    HybridTimestepInfo Info;
    struct _HybridTimestepEntry *Next;
} *HybridTimestepEntry;

typedef struct _Hybrid_RS_Stream
{
    CP_Services Svcs;
    void *CP_Stream;
    HybridConfig Config;
    char *Hostname;
    int Rank;
    int WriterCohortSize;
    size_t NumSubDPs;
    struct _HybridSubDPState *SubDPs;
    char **WriterHostnames;
    HybridContactInfo MyContact;
} *Hybrid_RS_Stream;

typedef struct _Hybrid_WS_Stream
{
    CP_Services Svcs;
    void *CP_Stream;
    HybridConfig Config;
    int Rank;
    char *Hostname;
    size_t NumSubDPs;
    struct _HybridSubDPState *SubDPs;
    HybridTimestepEntry Timesteps;
    int HasTimestepInfo;
} *Hybrid_WS_Stream;

typedef struct _Hybrid_WSR_Stream
{
    Hybrid_WS_Stream WS_Stream;
    DP_WSR_Stream *SubWSR;
    CP_DP_Interface *Interfaces;
    size_t NumSubDPs;
    HybridContactInfo MyContact;
} *Hybrid_WSR_Stream;

typedef struct _HybridCompletionHandle
{
    CP_DP_Interface Interface;
    DP_CompletionHandle Handle;
    void *SubTimestepInfo;
} *HybridCompletionHandle;

typedef struct _HybridSubDPRegistry
{
    const char *Name;
    CP_DP_Interface (*Load)(void);
} *HybridSubDPRegistry;

extern CP_DP_Interface LoadEVpathDP();
#ifdef SST_HAVE_LIBFABRIC
extern CP_DP_Interface LoadRdmaDP();
#endif
#ifdef SST_HAVE_UCX
extern CP_DP_Interface LoadUcxDP();
#endif
#ifdef SST_HAVE_DAOS
extern CP_DP_Interface LoadDaosDP();
#endif
#ifdef SST_HAVE_MERCURY
extern CP_DP_Interface LoadMercuryDP();
#endif
#ifdef ADIOS2_HAVE_MPI
extern CP_DP_Interface LoadMpiDP();
#endif

static struct _HybridSubDPRegistry HybridRegistry[] = {{"evpath", LoadEVpathDP},
#ifdef SST_HAVE_LIBFABRIC
                                                       {"rdma", LoadRdmaDP},
                                                       {"libfabric", LoadRdmaDP},
#endif
#ifdef SST_HAVE_UCX
                                                       {"ucx", LoadUcxDP},
#endif
#ifdef SST_HAVE_DAOS
                                                       {"daos", LoadDaosDP},
#endif
#ifdef SST_HAVE_MERCURY
                                                       {"mercury", LoadMercuryDP},
#endif
#ifdef ADIOS2_HAVE_MPI
                                                       {"mpi", LoadMpiDP},
#endif
                                                       {NULL, NULL}};

static char *HybridDupHostname(struct _SstParams *Params)
{
    char Buffer[256] = {0};
    const char *Override = get_dataplane_config(Params, "SST_HYBRID_NODE_ID");
    if (Override && Override[0])
    {
        return strdup(Override);
    }
#ifndef _WIN32
    if (gethostname(Buffer, sizeof(Buffer) - 1) != 0)
    {
        strncpy(Buffer, "unknown", sizeof(Buffer) - 1);
    }
#else
    DWORD Size = sizeof(Buffer);
    if (!GetComputerNameA(Buffer, &Size))
    {
        strncpy(Buffer, "unknown", sizeof(Buffer) - 1);
    }
#endif
    return strdup(Buffer);
}

static char *HybridTrim(char *String)
{
    while (*String && isspace((unsigned char)*String))
    {
        String++;
    }
    char *End = String + strlen(String);
    while (End > String && isspace((unsigned char)End[-1]))
    {
        End--;
    }
    *End = '\0';
    return String;
}

static int HybridParseScope(char *Scope)
{
    if (!Scope || !Scope[0])
    {
        return HybridScopeAny;
    }
    if (strcasecmp(Scope, "local") == 0 || strcasecmp(Scope, "node") == 0 ||
        strcasecmp(Scope, "shm") == 0)
    {
        return HybridScopeLocal;
    }
    if (strcasecmp(Scope, "remote") == 0 || strcasecmp(Scope, "network") == 0 ||
        strcasecmp(Scope, "rdma") == 0)
    {
        return HybridScopeRemote;
    }
    return -1;
}

static int HybridHexValue(char Character)
{
    if (Character >= '0' && Character <= '9')
    {
        return Character - '0';
    }
    if (Character >= 'a' && Character <= 'f')
    {
        return Character - 'a' + 10;
    }
    if (Character >= 'A' && Character <= 'F')
    {
        return Character - 'A' + 10;
    }
    return -1;
}

static void HybridPercentDecode(char *String)
{
    char *Read = String;
    char *Write = String;
    while (*Read)
    {
        if (*Read == '%' && HybridHexValue(Read[1]) >= 0 && HybridHexValue(Read[2]) >= 0)
        {
            *Write++ = (char)((HybridHexValue(Read[1]) << 4) | HybridHexValue(Read[2]));
            Read += 3;
        }
        else
        {
            *Write++ = *Read++;
        }
    }
    *Write = '\0';
}

static CP_DP_Interface HybridLoadSubDP(const char *Name)
{
    for (int i = 0; HybridRegistry[i].Name; i++)
    {
        if (strcasecmp(Name, HybridRegistry[i].Name) == 0)
        {
            return HybridRegistry[i].Load();
        }
    }
    return NULL;
}

static void HybridFreeContact(HybridContactInfo Contact)
{
    if (!Contact)
    {
        return;
    }
    if (Contact->SubInfo)
    {
        for (int i = 0; i < Contact->NumSubDPs; i++)
        {
            free(Contact->SubInfo[i].Name);
            free(Contact->SubInfo[i].Address);
        }
        free(Contact->SubInfo);
    }
    free(Contact->Hostname);
    free(Contact);
}

static void HybridFreeTimestepInfo(HybridTimestepInfo Info)
{
    if (!Info)
    {
        return;
    }
    if (Info->SubInfo)
    {
        for (int i = 0; i < Info->NumSubInfos; i++)
        {
            free(Info->SubInfo[i].Name);
            free(Info->SubInfo[i].Address);
        }
        free(Info->SubInfo);
    }
    free(Info);
}

static void HybridConfigDestroy(HybridConfig Config)
{
    if (!Config)
    {
        return;
    }
    for (int i = 0; i < Config->NumSubDPs; i++)
    {
        free(Config->SubDPs[i].TypeName);
        free(Config->SubDPs[i].Name);
        SST_FreeDataPlaneParams(&Config->SubDPs[i].Params);
    }
    if (Config->FFS)
    {
        free_FFSContext(Config->FFS);
    }
    free(Config);
}

static int HybridAssignDefaultIds(CP_Services Svcs, void *CP_Stream, HybridConfig Config)
{
    for (int i = 0; i < Config->NumSubDPs; i++)
    {
        if (Config->SubDPs[i].Name)
        {
            continue;
        }
        int Ordinal = 0;
        for (int j = 0; j < i; j++)
        {
            if (strcasecmp(Config->SubDPs[j].TypeName, Config->SubDPs[i].TypeName) == 0)
            {
                Ordinal++;
            }
        }
        char Buffer[256];
        snprintf(Buffer, sizeof(Buffer), "%s%d", Config->SubDPs[i].TypeName, Ordinal);
        Config->SubDPs[i].Name = strdup(Buffer);
    }

    for (int i = 0; i < Config->NumSubDPs; i++)
    {
        for (int j = i + 1; j < Config->NumSubDPs; j++)
        {
            if (strcmp(Config->SubDPs[i].Name, Config->SubDPs[j].Name) == 0)
            {
                if (Svcs)
                {
                    Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                  "Hybrid DataPlane: duplicate sub-dataplane id \"%s\".\n",
                                  Config->SubDPs[i].Name);
                }
                return -1;
            }
        }
    }
    return 0;
}

static HybridConfig HybridConfigCreate(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params,
                                       int RegisterFormats)
{
    const char *ConfigString = get_dataplane_config(Params, "SST_HYBRID_DP");
    if (!ConfigString || !ConfigString[0])
    {
        return NULL;
    }

    HybridConfig Config = calloc(1, sizeof(*Config));
    char *Copy = strdup(ConfigString);
    char *Cursor = Copy;
    while (Cursor && *Cursor && Config->NumSubDPs < HYBRID_MAX_SUB_DPS)
    {
        char *Separator = strchr(Cursor, ';');
        if (Separator)
        {
            *Separator = '\0';
        }
        char *Entry = HybridTrim(Cursor);
        if (Entry[0])
        {
            char *Options = strchr(Entry, '?');
            if (Options)
            {
                *Options++ = '\0';
            }
            char *TypeName = HybridTrim(Entry);
            CP_DP_Interface Interface = HybridLoadSubDP(TypeName);
            if (!Interface)
            {
                if (Svcs)
                {
                    Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                  "Hybrid DataPlane: unknown sub-dataplane \"%s\".\n", TypeName);
                }
            }
            else
            {
                struct _SstParams Overrides = {0};
                char *InstanceId = NULL;
                enum HybridSubDPScope Scope = HybridScopeAny;
                char *OptionCursor = Options;
                while (OptionCursor && *OptionCursor)
                {
                    char *Ampersand = strchr(OptionCursor, '&');
                    if (Ampersand)
                    {
                        *Ampersand = '\0';
                    }
                    char *Option = HybridTrim(OptionCursor);
                    if (Option[0])
                    {
                        char *Equals = strchr(Option, '=');
                        if (!Equals)
                        {
                            if (Svcs)
                            {
                                Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                              "Hybrid DataPlane: ignoring malformed option "
                                              "\"%s\".\n",
                                              Option);
                            }
                        }
                        else
                        {
                            *Equals++ = '\0';
                            char *Key = HybridTrim(Option);
                            char *Value = HybridTrim(Equals);
                            HybridPercentDecode(Key);
                            HybridPercentDecode(Value);
                            if (strcasecmp(Key, "id") == 0)
                            {
                                free(InstanceId);
                                InstanceId = strdup(Value);
                            }
                            else if (strcasecmp(Key, "scope") == 0)
                            {
                                int ParsedScope = HybridParseScope(Value);
                                if (ParsedScope < 0)
                                {
                                    if (Svcs)
                                    {
                                        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                                      "Hybrid DataPlane: unknown scope \"%s\", "
                                                      "using \"any\".\n",
                                                      Value);
                                    }
                                }
                                else
                                {
                                    Scope = (enum HybridSubDPScope)ParsedScope;
                                }
                            }
                            else if (SST_AddDataPlaneParam(&Overrides, Key, Value) != 0)
                            {
                                if (Svcs)
                                {
                                    Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                                  "Hybrid DataPlane: failed to add option "
                                                  "\"%s\".\n",
                                                  Key);
                                }
                            }
                        }
                    }
                    OptionCursor = Ampersand ? Ampersand + 1 : NULL;
                }

                int Index = Config->NumSubDPs++;
                struct _HybridSubDPConfig *SubDP = &Config->SubDPs[Index];
                SubDP->Interface = Interface;
                SubDP->TypeName = strdup(TypeName);
                SubDP->Name = InstanceId;
                SubDP->Scope = Scope;

                struct _SstParams SubParams = {0};
                if (Params)
                {
                    SubParams = *Params;
                }
                SubParams.DataPlaneParamCount = 0;
                SubParams.DataPlaneParamKeys = NULL;
                SubParams.DataPlaneParamValues = NULL;
                SST_MergeDataPlaneParams(&SubParams, Params, &Overrides);
                SubDP->Params = SubParams;
                SST_FreeDataPlaneParams(&Overrides);
            }
        }
        Cursor = Separator ? Separator + 1 : NULL;
    }
    if (Cursor && *Cursor && Svcs)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Hybrid DataPlane: ignoring sub-dataplane entries beyond the maximum of "
                      "%d.\n",
                      HYBRID_MAX_SUB_DPS);
    }
    free(Copy);

    if (Config->NumSubDPs == 0)
    {
        HybridConfigDestroy(Config);
        return NULL;
    }
    if (HybridAssignDefaultIds(Svcs, CP_Stream, Config) != 0)
    {
        HybridConfigDestroy(Config);
        return NULL;
    }

    if (RegisterFormats)
    {
        Config->FFS = create_FFSContext();
        for (int i = 0; i < Config->NumSubDPs; i++)
        {
            CP_DP_Interface Interface = Config->SubDPs[i].Interface;
            if (Interface->ReaderContactFormats)
            {
                FFSTypeHandle Handle =
                    FFSset_fixed_target(Config->FFS, Interface->ReaderContactFormats);
                Config->ReaderContactFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
            }
            if (Interface->WriterContactFormats)
            {
                FFSTypeHandle Handle =
                    FFSset_fixed_target(Config->FFS, Interface->WriterContactFormats);
                Config->WriterContactFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
            }
            if (Interface->TimestepInfoFormats)
            {
                FFSTypeHandle Handle =
                    FFSset_fixed_target(Config->FFS, Interface->TimestepInfoFormats);
                Config->TimestepInfoFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
            }
        }
    }
    return Config;
}

static FMField HybridSubDPInfoList[] = {
    {"Name", "string", sizeof(char *), FMOffset(HybridSubDPInfo, Name)},
    {"Length", "integer", sizeof(int), FMOffset(HybridSubDPInfo, Length)},
    {"Available", "integer", sizeof(int), FMOffset(HybridSubDPInfo, Available)},
    {"Address", "char[Length]", sizeof(char), FMOffset(HybridSubDPInfo, Address)},
    {NULL, NULL, 0, 0}};

static FMField HybridReaderContactList[] = {
    {"NumSubDPs", "integer", sizeof(int), FMOffset(HybridContactInfo, NumSubDPs)},
    {"Hostname", "string", sizeof(char *), FMOffset(HybridContactInfo, Hostname)},
    {"SubInfo", "HybridSubDPInfo[NumSubDPs]", sizeof(struct _HybridSubDPInfo),
     FMOffset(HybridContactInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec HybridReaderContactStructs[] = {
    {"HybridReaderContactInfo", HybridReaderContactList, sizeof(struct _HybridContactInfo), NULL},
    {"HybridSubDPInfo", HybridSubDPInfoList, sizeof(struct _HybridSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static FMField HybridWriterContactList[] = {
    {"NumSubDPs", "integer", sizeof(int), FMOffset(HybridContactInfo, NumSubDPs)},
    {"Hostname", "string", sizeof(char *), FMOffset(HybridContactInfo, Hostname)},
    {"SubInfo", "HybridSubDPInfo[NumSubDPs]", sizeof(struct _HybridSubDPInfo),
     FMOffset(HybridContactInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec HybridWriterContactStructs[] = {
    {"HybridWriterContactInfo", HybridWriterContactList, sizeof(struct _HybridContactInfo), NULL},
    {"HybridSubDPInfo", HybridSubDPInfoList, sizeof(struct _HybridSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static FMField HybridTimestepInfoList[] = {
    {"NumSubInfos", "integer", sizeof(int), FMOffset(HybridTimestepInfo, NumSubInfos)},
    {"SubInfo", "HybridSubDPInfo[NumSubInfos]", sizeof(struct _HybridSubDPInfo),
     FMOffset(HybridTimestepInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec HybridTimestepInfoStructs[] = {
    {"HybridTimestepInfo", HybridTimestepInfoList, sizeof(struct _HybridTimestepInfo), NULL},
    {"HybridSubDPInfo", HybridSubDPInfoList, sizeof(struct _HybridSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static void HybridEncodeSubInfo(FMFormat Format, void *Info, HybridSubDPInfo Result)
{
    Result->Length = 0;
    Result->Address = NULL;
    if (!Format || !Info)
    {
        return;
    }
    FFSBuffer Buffer = create_FFSBuffer();
    size_t Size = 0;
    char *Encoded = FFSencode(Buffer, Format, Info, &Size);
    if (Encoded && Size)
    {
        Result->Address = malloc(Size);
        memcpy(Result->Address, Encoded, Size);
        Result->Length = (int)Size;
    }
    free_FFSBuffer(Buffer);
}

static void *HybridDecodeSubInfoReal(FFSContext Context, HybridSubDPInfo Info)
{
    if (!Info || !Info->Available || Info->Length <= 0 || !Info->Address)
    {
        return NULL;
    }
    if (!FFSTypeHandle_from_encode(Context, Info->Address))
    {
        return NULL;
    }
    size_t Size = FFS_est_decode_length(Context, Info->Address, (size_t)Info->Length);
    if (Size == (size_t)-1 || Size == 0)
    {
        Size = (size_t)Info->Length;
    }
    void *Buffer = malloc(Size);
    if (!Buffer)
    {
        return NULL;
    }
    if (!FFSdecode_to_buffer(Context, Info->Address, Buffer))
    {
        free(Buffer);
        return NULL;
    }
    return Buffer;
}

static HybridContactInfo HybridMakeContact(HybridConfig Config, FMFormat *Formats,
                                           void **ContactInfo, int *Available, const char *Hostname)
{
    int NumSubDPs = Config->NumSubDPs;
    HybridContactInfo Contact = calloc(1, sizeof(*Contact));
    Contact->NumSubDPs = NumSubDPs;
    Contact->Hostname = strdup(Hostname ? Hostname : "unknown");
    Contact->SubInfo = calloc(NumSubDPs, sizeof(struct _HybridSubDPInfo));
    for (int i = 0; i < NumSubDPs; i++)
    {
        Contact->SubInfo[i].Name = strdup(Config->SubDPs[i].Name);
        Contact->SubInfo[i].Available = Available[i];
        if (Available[i])
        {
            HybridEncodeSubInfo(Formats[i], ContactInfo[i], &Contact->SubInfo[i]);
            if (Formats[i] && ContactInfo[i] && Contact->SubInfo[i].Length == 0)
            {
                Contact->SubInfo[i].Available = 0;
            }
        }
    }
    return Contact;
}

static int HybridFindSlot(HybridSubDPInfo Slots, int NumSlots, const char *Name)
{
    if (!Slots || !Name)
    {
        return -1;
    }
    for (int i = 0; i < NumSlots; i++)
    {
        if (Slots[i].Name && strcmp(Slots[i].Name, Name) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int HybridSameHost(Hybrid_RS_Stream Stream, int Rank)
{
    if (!Stream->Hostname)
    {
        return 0;
    }
    if (Rank < 0 || Rank >= Stream->WriterCohortSize || !Stream->WriterHostnames)
    {
        return 0;
    }
    if (!Stream->WriterHostnames[Rank])
    {
        return 0;
    }
    return strcmp(Stream->Hostname, Stream->WriterHostnames[Rank]) == 0;
}

static int HybridSubDPIsCandidate(Hybrid_RS_Stream Stream, size_t Index, int SameHost)
{
    if (!Stream->SubDPs[Index].Ready || !Stream->SubDPs[Index].RS_Stream)
    {
        return 0;
    }
    if (!Stream->SubDPs[Index].Interface->readRemoteMemory)
    {
        return 0;
    }
    switch (Stream->SubDPs[Index].Scope)
    {
    case HybridScopeLocal:
        return SameHost;
    case HybridScopeRemote:
        return !SameHost;
    default:
        return 1;
    }
}

static DP_RS_Stream HybridInitReader(CP_Services Svcs, void *CP_Stream, void **ReaderContactInfoPtr,
                                     struct _SstParams *Params, attr_list WriterContact,
                                     SstStats Stats)
{
    HybridConfig Config = HybridConfigCreate(Svcs, CP_Stream, Params, 1);
    if (!Config)
    {
        return NULL;
    }

    Hybrid_RS_Stream Stream = calloc(1, sizeof(*Stream));
    Stream->Svcs = Svcs;
    Stream->CP_Stream = CP_Stream;
    Stream->Config = Config;
    Stream->Hostname = HybridDupHostname(Params);
    Stream->NumSubDPs = (size_t)Config->NumSubDPs;
    Stream->SubDPs = calloc(Stream->NumSubDPs, sizeof(*Stream->SubDPs));

    void **ContactInfo = calloc(Stream->NumSubDPs, sizeof(*ContactInfo));
    int *Available = calloc(Stream->NumSubDPs, sizeof(*Available));
    int NumAvailable = 0;

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Interface = Config->SubDPs[i].Interface;
        SubDP->Name = Config->SubDPs[i].Name;
        SubDP->Scope = Config->SubDPs[i].Scope;
        if (SubDP->Interface->initReader)
        {
            SubDP->RS_Stream = SubDP->Interface->initReader(
                Svcs, CP_Stream, &ContactInfo[i], &Config->SubDPs[i].Params, WriterContact, Stats);
        }
        if (SubDP->RS_Stream)
        {
            SubDP->Ready = 1;
            Available[i] = 1;
            NumAvailable++;
        }
    }

    if (NumAvailable == 0)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Hybrid DataPlane: none of the configured sub-dataplanes initialized "
                      "successfully on the reader side.\n");
        free(ContactInfo);
        free(Available);
        free(Stream->SubDPs);
        free(Stream->Hostname);
        HybridConfigDestroy(Config);
        free(Stream);
        return NULL;
    }

    Stream->MyContact = HybridMakeContact(Config, Config->ReaderContactFormats, ContactInfo,
                                          Available, Stream->Hostname);
    free(ContactInfo);
    free(Available);

    *ReaderContactInfoPtr = Stream->MyContact;
    return Stream;
}

static void HybridDestroyReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    if (!Stream)
    {
        return;
    }
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        if (Stream->SubDPs[i].RS_Stream && Stream->SubDPs[i].Interface->destroyReader)
        {
            Stream->SubDPs[i].Interface->destroyReader(Svcs, Stream->SubDPs[i].RS_Stream);
        }
    }
    if (Stream->WriterHostnames)
    {
        for (int i = 0; i < Stream->WriterCohortSize; i++)
        {
            free(Stream->WriterHostnames[i]);
        }
        free(Stream->WriterHostnames);
    }
    HybridFreeContact(Stream->MyContact);
    HybridConfigDestroy(Stream->Config);
    free(Stream->SubDPs);
    free(Stream->Hostname);
    free(Stream);
}

static DP_WS_Stream HybridInitWriter(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params,
                                     attr_list DPAttrs, SstStats Stats)
{
    HybridConfig Config = HybridConfigCreate(Svcs, CP_Stream, Params, 1);
    if (!Config)
    {
        return NULL;
    }

    Hybrid_WS_Stream Stream = calloc(1, sizeof(*Stream));
    Stream->Svcs = Svcs;
    Stream->CP_Stream = CP_Stream;
    Stream->Config = Config;
    Stream->Hostname = HybridDupHostname(Params);
    Stream->NumSubDPs = (size_t)Config->NumSubDPs;
    Stream->SubDPs = calloc(Stream->NumSubDPs, sizeof(*Stream->SubDPs));

    int NumAvailable = 0;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Interface = Config->SubDPs[i].Interface;
        SubDP->Name = Config->SubDPs[i].Name;
        SubDP->Scope = Config->SubDPs[i].Scope;
        if (SubDP->Interface->initWriter)
        {
            SubDP->WS_Stream = SubDP->Interface->initWriter(
                Svcs, CP_Stream, &Config->SubDPs[i].Params, DPAttrs, Stats);
        }
        if (SubDP->WS_Stream)
        {
            SubDP->Ready = 1;
            NumAvailable++;
            if (SubDP->Interface->TimestepInfoFormats)
            {
                Stream->HasTimestepInfo = 1;
            }
        }
    }

    if (NumAvailable == 0)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Hybrid DataPlane: none of the configured sub-dataplanes initialized "
                      "successfully on the writer side.\n");
        free(Stream->SubDPs);
        free(Stream->Hostname);
        HybridConfigDestroy(Config);
        free(Stream);
        return NULL;
    }
    return Stream;
}

static DP_WSR_Stream HybridInitWriterPerReader(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                               int ReaderCohortSize, CP_PeerCohort PeerCohort,
                                               void **ProvidedReaderInfo,
                                               void **WriterContactInfoPtr)
{
    Hybrid_WS_Stream Stream = (Hybrid_WS_Stream)WS_Stream_v;
    Hybrid_WSR_Stream WSR = calloc(1, sizeof(*WSR));
    WSR->WS_Stream = Stream;
    WSR->NumSubDPs = Stream->NumSubDPs;
    WSR->SubWSR = calloc(Stream->NumSubDPs, sizeof(*WSR->SubWSR));
    WSR->Interfaces = calloc(Stream->NumSubDPs, sizeof(*WSR->Interfaces));
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        WSR->Interfaces[i] = Stream->SubDPs[i].Interface;
    }

    void **ContactInfo = calloc(Stream->NumSubDPs, sizeof(*ContactInfo));
    int *Available = calloc(Stream->NumSubDPs, sizeof(*Available));

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (!SubDP->Ready || !SubDP->WS_Stream || !SubDP->Interface->initWriterPerReader)
        {
            continue;
        }

        int AllAvailable = 1;
        for (int j = 0; j < ReaderCohortSize; j++)
        {
            HybridContactInfo ReaderContact = (HybridContactInfo)ProvidedReaderInfo[j];
            int Slot = ReaderContact ? HybridFindSlot(ReaderContact->SubInfo,
                                                      ReaderContact->NumSubDPs, SubDP->Name)
                                     : -1;
            if (Slot < 0 || !ReaderContact->SubInfo[Slot].Available)
            {
                AllAvailable = 0;
                break;
            }
        }
        if (!AllAvailable)
        {
            continue;
        }

        void **ReaderInfo = calloc(ReaderCohortSize, sizeof(*ReaderInfo));
        int DecodeOk = 1;
        for (int j = 0; j < ReaderCohortSize; j++)
        {
            HybridContactInfo ReaderContact = (HybridContactInfo)ProvidedReaderInfo[j];
            int Slot =
                HybridFindSlot(ReaderContact->SubInfo, ReaderContact->NumSubDPs, SubDP->Name);
            ReaderInfo[j] =
                HybridDecodeSubInfoReal(Stream->Config->FFS, &ReaderContact->SubInfo[Slot]);
            if (ReaderContact->SubInfo[Slot].Length > 0 && !ReaderInfo[j])
            {
                DecodeOk = 0;
                break;
            }
        }
        if (!DecodeOk)
        {
            for (int j = 0; j < ReaderCohortSize; j++)
            {
                free(ReaderInfo[j]);
            }
            free(ReaderInfo);
            continue;
        }

        DP_WSR_Stream SubWSR = SubDP->Interface->initWriterPerReader(
            Svcs, SubDP->WS_Stream, ReaderCohortSize, PeerCohort, ReaderInfo, &ContactInfo[i]);

        for (int j = 0; j < ReaderCohortSize; j++)
        {
            free(ReaderInfo[j]);
        }
        free(ReaderInfo);

        if (SubWSR)
        {
            WSR->SubWSR[i] = SubWSR;
            SubDP->Ready = 1;
            Available[i] = 1;
        }
    }

    HybridContactInfo Contact =
        HybridMakeContact(Stream->Config, Stream->Config->WriterContactFormats, ContactInfo,
                          Available, Stream->Hostname);
    free(ContactInfo);
    free(Available);

    WSR->MyContact = Contact;
    *WriterContactInfoPtr = Contact;
    return WSR;
}

static void HybridProvideWriterDataToReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                            int WriterCohortSize, CP_PeerCohort PeerCohort,
                                            void **ProvidedWriterInfo)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    if (Stream->WriterHostnames)
    {
        for (int i = 0; i < Stream->WriterCohortSize; i++)
        {
            free(Stream->WriterHostnames[i]);
        }
        free(Stream->WriterHostnames);
    }
    Stream->WriterCohortSize = WriterCohortSize;
    Stream->WriterHostnames = calloc(WriterCohortSize, sizeof(*Stream->WriterHostnames));
    for (int j = 0; j < WriterCohortSize; j++)
    {
        HybridContactInfo WriterContact = (HybridContactInfo)ProvidedWriterInfo[j];
        if (WriterContact && WriterContact->Hostname)
        {
            Stream->WriterHostnames[j] = strdup(WriterContact->Hostname);
        }
    }

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Ready = 0;
        if (!SubDP->RS_Stream || !SubDP->Interface->provideWriterDataToReader)
        {
            continue;
        }

        int AllAvailable = 1;
        for (int j = 0; j < WriterCohortSize; j++)
        {
            HybridContactInfo WriterContact = (HybridContactInfo)ProvidedWriterInfo[j];
            int Slot = WriterContact ? HybridFindSlot(WriterContact->SubInfo,
                                                      WriterContact->NumSubDPs, SubDP->Name)
                                     : -1;
            if (Slot < 0 || !WriterContact->SubInfo[Slot].Available)
            {
                AllAvailable = 0;
                break;
            }
        }
        if (!AllAvailable)
        {
            continue;
        }

        void **WriterInfo = calloc(WriterCohortSize, sizeof(*WriterInfo));
        int DecodeOk = 1;
        for (int j = 0; j < WriterCohortSize; j++)
        {
            HybridContactInfo WriterContact = (HybridContactInfo)ProvidedWriterInfo[j];
            int Slot =
                HybridFindSlot(WriterContact->SubInfo, WriterContact->NumSubDPs, SubDP->Name);
            WriterInfo[j] =
                HybridDecodeSubInfoReal(Stream->Config->FFS, &WriterContact->SubInfo[Slot]);
            if (WriterContact->SubInfo[Slot].Length > 0 && !WriterInfo[j])
            {
                DecodeOk = 0;
                break;
            }
        }
        if (!DecodeOk)
        {
            for (int j = 0; j < WriterCohortSize; j++)
            {
                free(WriterInfo[j]);
            }
            free(WriterInfo);
            continue;
        }

        SubDP->Interface->provideWriterDataToReader(Svcs, SubDP->RS_Stream, WriterCohortSize,
                                                    PeerCohort, WriterInfo);

        for (int j = 0; j < WriterCohortSize; j++)
        {
            free(WriterInfo[j]);
        }
        free(WriterInfo);
        SubDP->Ready = 1;
    }
}

static void *HybridReadRemoteMemory(CP_Services Svcs, DP_RS_Stream RS_Stream_v, int Rank,
                                    size_t Timestep, size_t Offset, size_t Length, void *Buffer,
                                    void *DP_TimestepInfo)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    HybridTimestepInfo TimestepInfo = (HybridTimestepInfo)DP_TimestepInfo;
    int SameHost = HybridSameHost(Stream, Rank);

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        if (!HybridSubDPIsCandidate(Stream, i, SameHost))
        {
            continue;
        }

        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->Interface->getPeerRouteStatus &&
            SubDP->Interface->getPeerRouteStatus(Svcs, SubDP->RS_Stream, Rank) ==
                DPPeerRouteNoRoute)
        {
            continue;
        }
        void *SubTimestepInfo = NULL;
        if (SubDP->Interface->TimestepInfoFormats)
        {
            int Slot = TimestepInfo ? HybridFindSlot(TimestepInfo->SubInfo,
                                                     TimestepInfo->NumSubInfos, SubDP->Name)
                                    : -1;
            if (Slot < 0 || !TimestepInfo->SubInfo[Slot].Available)
            {
                continue;
            }
            SubTimestepInfo =
                HybridDecodeSubInfoReal(Stream->Config->FFS, &TimestepInfo->SubInfo[Slot]);
            if (!SubTimestepInfo)
            {
                continue;
            }
        }

        DP_CompletionHandle Handle = SubDP->Interface->readRemoteMemory(
            Svcs, SubDP->RS_Stream, Rank, Timestep, Offset, Length, Buffer, SubTimestepInfo);
        if (Handle)
        {
            HybridCompletionHandle Completion = malloc(sizeof(*Completion));
            Completion->Interface = SubDP->Interface;
            Completion->Handle = Handle;
            Completion->SubTimestepInfo = SubTimestepInfo;
            return Completion;
        }
        free(SubTimestepInfo);
    }
    return NULL;
}

static int HybridWaitForCompletion(CP_Services Svcs, void *Handle_v)
{
    HybridCompletionHandle Completion = (HybridCompletionHandle)Handle_v;
    if (!Completion)
    {
        return 0;
    }
    int Ret = 1;
    if (Completion->Interface && Completion->Interface->waitForCompletion)
    {
        Ret = Completion->Interface->waitForCompletion(Svcs, Completion->Handle);
    }
    free(Completion->SubTimestepInfo);
    free(Completion);
    return Ret;
}

static void HybridNotifyConnFailure(CP_Services Svcs, DP_RS_Stream RS_Stream_v, int FailedPeerRank)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->notifyConnFailure)
        {
            SubDP->Interface->notifyConnFailure(Svcs, SubDP->RS_Stream, FailedPeerRank);
        }
    }
}

static void HybridTimestepArrived(CP_Services Svcs, DP_RS_Stream RS_Stream_v, size_t Timestep,
                                  SstPreloadModeType PreloadMode)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->timestepArrived)
        {
            SubDP->Interface->timestepArrived(Svcs, SubDP->RS_Stream, Timestep, PreloadMode);
        }
    }
}

static void HybridReaderRegisterTimestep(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                         size_t Timestep, SstPreloadModeType PreloadMode)
{
    Hybrid_WSR_Stream WSR = (Hybrid_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->readerRegisterTimestep)
        {
            WSR->WS_Stream->SubDPs[i].Interface->readerRegisterTimestep(Svcs, WSR->SubWSR[i],
                                                                        Timestep, PreloadMode);
        }
    }
}

static void HybridReaderReleaseTimestep(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                        size_t Timestep)
{
    Hybrid_WSR_Stream WSR = (Hybrid_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->readerReleaseTimestep)
        {
            WSR->WS_Stream->SubDPs[i].Interface->readerReleaseTimestep(Svcs, WSR->SubWSR[i],
                                                                       Timestep);
        }
    }
}

static void HybridWSRReadPatternLocked(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                       size_t EffectiveTimestep)
{
    Hybrid_WSR_Stream WSR = (Hybrid_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->WSRreadPatternLocked)
        {
            WSR->WS_Stream->SubDPs[i].Interface->WSRreadPatternLocked(Svcs, WSR->SubWSR[i],
                                                                      EffectiveTimestep);
        }
    }
}

static void HybridRSReadPatternLocked(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                      size_t EffectiveTimestep)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->RSreadPatternLocked)
        {
            SubDP->Interface->RSreadPatternLocked(Svcs, SubDP->RS_Stream, EffectiveTimestep);
        }
    }
}

static void HybridRSReleaseTimestep(CP_Services Svcs, DP_RS_Stream RS_Stream_v, size_t Timestep)
{
    Hybrid_RS_Stream Stream = (Hybrid_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->RSReleaseTimestep)
        {
            SubDP->Interface->RSReleaseTimestep(Svcs, SubDP->RS_Stream, Timestep);
        }
    }
}

static void HybridProvideTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v, struct _SstData *Data,
                                  struct _SstData *LocalMetadata, size_t Timestep,
                                  void **TimestepInfoPtr)
{
    Hybrid_WS_Stream Stream = (Hybrid_WS_Stream)WS_Stream_v;
    HybridTimestepInfo Info = NULL;
    if (Stream->HasTimestepInfo)
    {
        Info = calloc(1, sizeof(*Info));
        Info->NumSubInfos = (int)Stream->NumSubDPs;
        Info->SubInfo = calloc(Stream->NumSubDPs, sizeof(struct _HybridSubDPInfo));
    }

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (Info)
        {
            Info->SubInfo[i].Name = strdup(Stream->Config->SubDPs[i].Name);
        }
        if (!SubDP->Ready || !SubDP->WS_Stream || !SubDP->Interface->provideTimestep)
        {
            continue;
        }
        void *SubInfo = NULL;
        SubDP->Interface->provideTimestep(Svcs, SubDP->WS_Stream, Data, LocalMetadata, Timestep,
                                          &SubInfo);
        if (Info)
        {
            HybridEncodeSubInfo(Stream->Config->TimestepInfoFormats[i], SubInfo, &Info->SubInfo[i]);
            if (SubDP->Interface->TimestepInfoFormats && Info->SubInfo[i].Length > 0)
            {
                Info->SubInfo[i].Available = 1;
            }
            else if (!SubDP->Interface->TimestepInfoFormats)
            {
                Info->SubInfo[i].Available = 1;
            }
        }
    }

    if (Info)
    {
        HybridTimestepEntry Entry = calloc(1, sizeof(*Entry));
        Entry->Timestep = Timestep;
        Entry->Info = Info;
        Entry->Next = Stream->Timesteps;
        Stream->Timesteps = Entry;
    }

    *TimestepInfoPtr = Info;
}

static void HybridReleaseTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v, size_t Timestep)
{
    Hybrid_WS_Stream Stream = (Hybrid_WS_Stream)WS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->Ready && SubDP->WS_Stream && SubDP->Interface->releaseTimestep)
        {
            SubDP->Interface->releaseTimestep(Svcs, SubDP->WS_Stream, Timestep);
        }
    }

    HybridTimestepEntry *Link = &Stream->Timesteps;
    while (*Link)
    {
        if ((*Link)->Timestep == Timestep)
        {
            HybridTimestepEntry Entry = *Link;
            *Link = Entry->Next;
            HybridFreeTimestepInfo(Entry->Info);
            free(Entry);
            break;
        }
        Link = &(*Link)->Next;
    }
}

static void HybridDestroyWriterPerReader(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v)
{
    Hybrid_WSR_Stream WSR = (Hybrid_WSR_Stream)WSR_Stream_v;
    if (!WSR)
    {
        return;
    }
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->Interfaces[i] && WSR->Interfaces[i]->destroyWriterPerReader)
        {
            WSR->Interfaces[i]->destroyWriterPerReader(Svcs, WSR->SubWSR[i]);
        }
    }
    free(WSR->SubWSR);
    free(WSR->Interfaces);
    HybridFreeContact(WSR->MyContact);
    free(WSR);
}

static void HybridDestroyWriter(CP_Services Svcs, DP_WS_Stream WS_Stream_v)
{
    Hybrid_WS_Stream Stream = (Hybrid_WS_Stream)WS_Stream_v;
    if (!Stream)
    {
        return;
    }
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        HybridSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->WS_Stream && SubDP->Interface->destroyWriter)
        {
            SubDP->Interface->destroyWriter(Svcs, SubDP->WS_Stream);
        }
    }
    HybridTimestepEntry Entry = Stream->Timesteps;
    while (Entry)
    {
        HybridTimestepEntry Next = Entry->Next;
        HybridFreeTimestepInfo(Entry->Info);
        free(Entry);
        Entry = Next;
    }
    HybridConfigDestroy(Stream->Config);
    free(Stream->SubDPs);
    free(Stream->Hostname);
    free(Stream);
}

static int HybridGetPriority(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params)
{
    HybridConfig Config = HybridConfigCreate(Svcs, CP_Stream, Params, 0);
    if (!Config)
    {
        return -1;
    }

    int NumUsable = 0;
    for (int i = 0; i < Config->NumSubDPs; i++)
    {
        CP_DP_Interface Interface = Config->SubDPs[i].Interface;
        if (!Interface->getPriority ||
            Interface->getPriority(Svcs, CP_Stream, &Config->SubDPs[i].Params) >= 0)
        {
            NumUsable++;
        }
    }
    HybridConfigDestroy(Config);

    if (NumUsable == 0)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Hybrid DataPlane: none of the configured sub-dataplanes is usable.\n");
        return -1;
    }
    return 1000;
}

static void HybridUnGetPriority(CP_Services Svcs, void *CP_Stream)
{
    Svcs->verbose(CP_Stream, DPPerStepVerbose, "Hybrid Dataplane unloading\n");
}

static struct _CP_DP_Interface HybridDPInterface = {0};

extern CP_DP_Interface LoadHybridDP()
{
    HybridDPInterface.DPName = "hybrid";
    HybridDPInterface.ReaderContactFormats = HybridReaderContactStructs;
    HybridDPInterface.WriterContactFormats = HybridWriterContactStructs;
    HybridDPInterface.TimestepInfoFormats = HybridTimestepInfoStructs;
    HybridDPInterface.initReader = HybridInitReader;
    HybridDPInterface.initWriter = HybridInitWriter;
    HybridDPInterface.initWriterPerReader = HybridInitWriterPerReader;
    HybridDPInterface.provideWriterDataToReader = HybridProvideWriterDataToReader;
    HybridDPInterface.readRemoteMemory = (CP_DP_ReadRemoteMemoryFunc)HybridReadRemoteMemory;
    HybridDPInterface.waitForCompletion = HybridWaitForCompletion;
    HybridDPInterface.notifyConnFailure = HybridNotifyConnFailure;
    HybridDPInterface.provideTimestep = (CP_DP_ProvideTimestepFunc)HybridProvideTimestep;
    HybridDPInterface.readerRegisterTimestep = HybridReaderRegisterTimestep;
    HybridDPInterface.releaseTimestep = (CP_DP_ReleaseTimestepFunc)HybridReleaseTimestep;
    HybridDPInterface.readerReleaseTimestep = HybridReaderReleaseTimestep;
    HybridDPInterface.WSRreadPatternLocked = HybridWSRReadPatternLocked;
    HybridDPInterface.RSreadPatternLocked = HybridRSReadPatternLocked;
    HybridDPInterface.RSReleaseTimestep = HybridRSReleaseTimestep;
    HybridDPInterface.timestepArrived = (CP_DP_ReaderTimestepArrivalFunc)HybridTimestepArrived;
    HybridDPInterface.destroyReader = HybridDestroyReader;
    HybridDPInterface.destroyWriter = HybridDestroyWriter;
    HybridDPInterface.destroyWriterPerReader = HybridDestroyWriterPerReader;
    HybridDPInterface.getPriority = HybridGetPriority;
    HybridDPInterface.unGetPriority = HybridUnGetPriority;
    return &HybridDPInterface;
}
