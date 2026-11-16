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
#include "dp_interface.h"

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strdup _strdup
#endif

#define PARALLEL_MAX_SUB_DPS 8

enum ParallelSubDPScope
{
    ParallelScopeAny = 0,
    ParallelScopeLocal,
    ParallelScopeRemote
};

typedef struct _ParallelSubDPConfig
{
    CP_DP_Interface Interface;
    const char *Name;
    enum ParallelSubDPScope Scope;
} *ParallelSubDPConfig;

typedef struct _ParallelGlobalInfo
{
    int Attempted;
    int NumSubDPs;
    struct _ParallelSubDPConfig SubDPs[PARALLEL_MAX_SUB_DPS];
    FMContext FM;
    FFSContext FFS;
    FMFormat ReaderContactFormats[PARALLEL_MAX_SUB_DPS];
    FMFormat WriterContactFormats[PARALLEL_MAX_SUB_DPS];
    FMFormat TimestepInfoFormats[PARALLEL_MAX_SUB_DPS];
} *ParallelGlobalInfo;

static struct _ParallelGlobalInfo ParallelGlobal = {0};

typedef struct _ParallelSubDPInfo
{
    char *Name;
    int Length;
    int Available;
    char *Address;
} *ParallelSubDPInfo;

typedef struct _ParallelContactInfo
{
    int NumSubDPs;
    char *Hostname;
    struct _ParallelSubDPInfo *SubInfo;
} *ParallelContactInfo;

typedef struct _ParallelTimestepInfo
{
    int NumSubInfos;
    struct _ParallelSubDPInfo *SubInfo;
} *ParallelTimestepInfo;

typedef struct _ParallelSubDPState
{
    CP_DP_Interface Interface;
    const char *Name;
    enum ParallelSubDPScope Scope;
    DP_RS_Stream RS_Stream;
    DP_WS_Stream WS_Stream;
    DP_WSR_Stream WSR_Stream;
    int Ready;
} *ParallelSubDPState;

typedef struct _ParallelTimestepEntry
{
    size_t Timestep;
    ParallelTimestepInfo Info;
    struct _ParallelTimestepEntry *Next;
} *ParallelTimestepEntry;

typedef struct _Parallel_RS_Stream
{
    CP_Services Svcs;
    void *CP_Stream;
    char *Hostname;
    int Rank;
    int WriterCohortSize;
    size_t NumSubDPs;
    struct _ParallelSubDPState *SubDPs;
    char **WriterHostnames;
    ParallelContactInfo MyContact;
} *Parallel_RS_Stream;

typedef struct _Parallel_WS_Stream
{
    CP_Services Svcs;
    void *CP_Stream;
    int Rank;
    char *Hostname;
    size_t NumSubDPs;
    struct _ParallelSubDPState *SubDPs;
    ParallelTimestepEntry Timesteps;
} *Parallel_WS_Stream;

typedef struct _Parallel_WSR_Stream
{
    Parallel_WS_Stream WS_Stream;
    DP_WSR_Stream *SubWSR;
    size_t NumSubDPs;
    ParallelContactInfo MyContact;
} *Parallel_WSR_Stream;

typedef struct _ParallelCompletionHandle
{
    CP_DP_Interface Interface;
    DP_CompletionHandle Handle;
} *ParallelCompletionHandle;

typedef struct _ParallelSubDPRegistry
{
    const char *Name;
    CP_DP_Interface (*Load)(void);
} *ParallelSubDPRegistry;

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

static struct _ParallelSubDPRegistry ParallelRegistry[] = {{"evpath", LoadEVpathDP},
#ifdef SST_HAVE_LIBFABRIC
                                                           {"rdma", LoadRdmaDP},
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

static char *ParallelDupHostname()
{
    char Buffer[256] = {0};
    const char *Override = getenv("SST_PARALLEL_NODE_ID");
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

static char *ParallelTrim(char *String)
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

static int ParallelParseScope(char *Scope)
{
    if (!Scope || !Scope[0])
    {
        return ParallelScopeAny;
    }
    if (strcasecmp(Scope, "local") == 0 || strcasecmp(Scope, "node") == 0 ||
        strcasecmp(Scope, "shm") == 0)
    {
        return ParallelScopeLocal;
    }
    if (strcasecmp(Scope, "remote") == 0 || strcasecmp(Scope, "network") == 0 ||
        strcasecmp(Scope, "rdma") == 0)
    {
        return ParallelScopeRemote;
    }
    return ParallelScopeAny;
}

static CP_DP_Interface ParallelLoadSubDP(const char *Name)
{
    for (int i = 0; ParallelRegistry[i].Name; i++)
    {
        if (strcasecmp(Name, ParallelRegistry[i].Name) == 0)
        {
            return ParallelRegistry[i].Load();
        }
    }
    return NULL;
}

static void ParallelFreeContact(ParallelContactInfo Contact)
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

static void ParallelFreeTimestepInfo(ParallelTimestepInfo Info)
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

static void ParallelEnsureConfig(CP_Services Svcs, void *CP_Stream)
{
    if (ParallelGlobal.Attempted)
    {
        return;
    }
    ParallelGlobal.Attempted = 1;

    const char *Config = getenv("SST_PARALLEL_DP");
    if (!Config || !Config[0])
    {
        Config = getenv("SST_VIRTUAL_DP");
    }
    if (!Config || !Config[0])
    {
        return;
    }

    char *Copy = strdup(Config);
    char *Cursor = Copy;
    while (Cursor && *Cursor && ParallelGlobal.NumSubDPs < PARALLEL_MAX_SUB_DPS)
    {
        char *Comma = strchr(Cursor, ',');
        if (Comma)
        {
            *Comma = '\0';
        }
        char *Entry = ParallelTrim(Cursor);
        if (Entry[0])
        {
            char *Scope = NULL;
            char *Separator = strchr(Entry, '@');
            if (!Separator)
            {
                Separator = strchr(Entry, ':');
            }
            if (Separator)
            {
                *Separator = '\0';
                Scope = ParallelTrim(Separator + 1);
                Entry = ParallelTrim(Entry);
            }
            CP_DP_Interface Interface = ParallelLoadSubDP(Entry);
            if (Interface)
            {
                int Duplicate = 0;
                for (int i = 0; i < ParallelGlobal.NumSubDPs; i++)
                {
                    if (ParallelGlobal.SubDPs[i].Interface == Interface)
                    {
                        Duplicate = 1;
                        break;
                    }
                }
                if (!Duplicate)
                {
                    int Index = ParallelGlobal.NumSubDPs++;
                    ParallelGlobal.SubDPs[Index].Interface = Interface;
                    ParallelGlobal.SubDPs[Index].Name = strdup(Entry);
                    ParallelGlobal.SubDPs[Index].Scope = ParallelParseScope(Scope);
                }
                else if (Svcs)
                {
                    Svcs->verbose(CP_Stream, DPCriticalVerbose,
                                  "Parallel DataPlane: ignoring duplicate sub-dataplane \"%s\".\n",
                                  Entry);
                }
            }
            else if (Svcs)
            {
                Svcs->verbose(CP_Stream, DPCriticalVerbose,
                              "Parallel DataPlane: unknown sub-dataplane \"%s\".\n", Entry);
            }
        }
        Cursor = Comma ? Comma + 1 : NULL;
    }
    free(Copy);

    if (ParallelGlobal.NumSubDPs == 0)
    {
        return;
    }

    ParallelGlobal.FM = create_FMcontext();
    ParallelGlobal.FFS = create_FFSContext_FM(ParallelGlobal.FM);
    for (int i = 0; i < ParallelGlobal.NumSubDPs; i++)
    {
        CP_DP_Interface Interface = ParallelGlobal.SubDPs[i].Interface;
        if (Interface->ReaderContactFormats)
        {
            FFSTypeHandle Handle =
                FFSset_fixed_target(ParallelGlobal.FFS, Interface->ReaderContactFormats);
            ParallelGlobal.ReaderContactFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
        }
        if (Interface->WriterContactFormats)
        {
            FFSTypeHandle Handle =
                FFSset_fixed_target(ParallelGlobal.FFS, Interface->WriterContactFormats);
            ParallelGlobal.WriterContactFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
        }
        if (Interface->TimestepInfoFormats)
        {
            FFSTypeHandle Handle =
                FFSset_fixed_target(ParallelGlobal.FFS, Interface->TimestepInfoFormats);
            ParallelGlobal.TimestepInfoFormats[i] = Handle ? FMFormat_of_original(Handle) : NULL;
        }
    }
}

static FMField ParallelSubDPInfoList[] = {
    {"Name", "string", sizeof(char *), FMOffset(ParallelSubDPInfo, Name)},
    {"Length", "integer", sizeof(int), FMOffset(ParallelSubDPInfo, Length)},
    {"Available", "integer", sizeof(int), FMOffset(ParallelSubDPInfo, Available)},
    {"Address", "char[Length]", sizeof(char), FMOffset(ParallelSubDPInfo, Address)},
    {NULL, NULL, 0, 0}};

static FMField ParallelReaderContactList[] = {
    {"NumSubDPs", "integer", sizeof(int), FMOffset(ParallelContactInfo, NumSubDPs)},
    {"Hostname", "string", sizeof(char *), FMOffset(ParallelContactInfo, Hostname)},
    {"SubInfo", "ParallelSubDPInfo[NumSubDPs]", sizeof(struct _ParallelSubDPInfo),
     FMOffset(ParallelContactInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelReaderContactStructs[] = {
    {"ParallelReaderContactInfo", ParallelReaderContactList, sizeof(struct _ParallelContactInfo),
     NULL},
    {"ParallelSubDPInfo", ParallelSubDPInfoList, sizeof(struct _ParallelSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static FMField ParallelWriterContactList[] = {
    {"NumSubDPs", "integer", sizeof(int), FMOffset(ParallelContactInfo, NumSubDPs)},
    {"Hostname", "string", sizeof(char *), FMOffset(ParallelContactInfo, Hostname)},
    {"SubInfo", "ParallelSubDPInfo[NumSubDPs]", sizeof(struct _ParallelSubDPInfo),
     FMOffset(ParallelContactInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelWriterContactStructs[] = {
    {"ParallelWriterContactInfo", ParallelWriterContactList, sizeof(struct _ParallelContactInfo),
     NULL},
    {"ParallelSubDPInfo", ParallelSubDPInfoList, sizeof(struct _ParallelSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static FMField ParallelTimestepInfoList[] = {
    {"NumSubInfos", "integer", sizeof(int), FMOffset(ParallelTimestepInfo, NumSubInfos)},
    {"SubInfo", "ParallelSubDPInfo[NumSubInfos]", sizeof(struct _ParallelSubDPInfo),
     FMOffset(ParallelTimestepInfo, SubInfo)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelTimestepInfoStructs[] = {
    {"ParallelTimestepInfo", ParallelTimestepInfoList, sizeof(struct _ParallelTimestepInfo), NULL},
    {"ParallelSubDPInfo", ParallelSubDPInfoList, sizeof(struct _ParallelSubDPInfo), NULL},
    {NULL, NULL, 0, NULL}};

static void ParallelEncodeSubInfo(FMFormat Format, void *Info, ParallelSubDPInfo Result)
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

static void *ParallelDecodeSubInfoReal(FFSContext Context, ParallelSubDPInfo Info)
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

static ParallelContactInfo ParallelMakeContact(FMFormat *Formats, void **ContactInfo,
                                               int *Available, const char *Hostname, int NumSubDPs)
{
    ParallelContactInfo Contact = calloc(1, sizeof(*Contact));
    Contact->NumSubDPs = NumSubDPs;
    Contact->Hostname = strdup(Hostname ? Hostname : "unknown");
    Contact->SubInfo = calloc(NumSubDPs, sizeof(struct _ParallelSubDPInfo));
    for (int i = 0; i < NumSubDPs; i++)
    {
        Contact->SubInfo[i].Name = strdup(ParallelGlobal.SubDPs[i].Name);
        Contact->SubInfo[i].Available = Available[i];
        if (Available[i])
        {
            ParallelEncodeSubInfo(Formats[i], ContactInfo[i], &Contact->SubInfo[i]);
            if (Formats[i] && ContactInfo[i] && Contact->SubInfo[i].Length == 0)
            {
                Contact->SubInfo[i].Available = 0;
            }
        }
    }
    return Contact;
}

static int ParallelFindSlot(ParallelSubDPInfo Slots, int NumSlots, const char *Name)
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

static int ParallelSameHost(Parallel_RS_Stream Stream, int Rank)
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

static int ParallelSubDPIsCandidate(Parallel_RS_Stream Stream, size_t Index, int SameHost)
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
    case ParallelScopeLocal:
        return SameHost;
    case ParallelScopeRemote:
        return !SameHost;
    default:
        return 1;
    }
}

static DP_RS_Stream ParallelInitReader(CP_Services Svcs, void *CP_Stream,
                                       void **ReaderContactInfoPtr, struct _SstParams *Params,
                                       attr_list WriterContact, SstStats Stats)
{
    ParallelEnsureConfig(Svcs, CP_Stream);
    if (ParallelGlobal.NumSubDPs == 0)
    {
        return NULL;
    }

    Parallel_RS_Stream Stream = calloc(1, sizeof(*Stream));
    Stream->Svcs = Svcs;
    Stream->CP_Stream = CP_Stream;
    Stream->Hostname = ParallelDupHostname();
    Stream->NumSubDPs = (size_t)ParallelGlobal.NumSubDPs;
    Stream->SubDPs = calloc(Stream->NumSubDPs, sizeof(*Stream->SubDPs));

    void **ContactInfo = calloc(Stream->NumSubDPs, sizeof(*ContactInfo));
    int *Available = calloc(Stream->NumSubDPs, sizeof(*Available));
    int NumAvailable = 0;

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Interface = ParallelGlobal.SubDPs[i].Interface;
        SubDP->Name = ParallelGlobal.SubDPs[i].Name;
        SubDP->Scope = ParallelGlobal.SubDPs[i].Scope;
        if (SubDP->Interface->initReader)
        {
            SubDP->RS_Stream = SubDP->Interface->initReader(Svcs, CP_Stream, &ContactInfo[i],
                                                            Params, WriterContact, Stats);
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
                      "Parallel DataPlane: none of the configured sub-dataplanes initialized "
                      "successfully on the reader side.\n");
        free(ContactInfo);
        free(Available);
        free(Stream->SubDPs);
        free(Stream->Hostname);
        free(Stream);
        return NULL;
    }

    Stream->MyContact = ParallelMakeContact(ParallelGlobal.ReaderContactFormats, ContactInfo,
                                            Available, Stream->Hostname, (int)Stream->NumSubDPs);
    free(ContactInfo);
    free(Available);

    *ReaderContactInfoPtr = Stream->MyContact;
    return Stream;
}

static void ParallelDestroyReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
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
    ParallelFreeContact(Stream->MyContact);
    free(Stream->SubDPs);
    free(Stream->Hostname);
    free(Stream);
}

static DP_WS_Stream ParallelInitWriter(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params,
                                       attr_list DPAttrs, SstStats Stats)
{
    ParallelEnsureConfig(Svcs, CP_Stream);
    if (ParallelGlobal.NumSubDPs == 0)
    {
        return NULL;
    }

    Parallel_WS_Stream Stream = calloc(1, sizeof(*Stream));
    Stream->Svcs = Svcs;
    Stream->CP_Stream = CP_Stream;
    Stream->Hostname = ParallelDupHostname();
    Stream->NumSubDPs = (size_t)ParallelGlobal.NumSubDPs;
    Stream->SubDPs = calloc(Stream->NumSubDPs, sizeof(*Stream->SubDPs));

    int NumAvailable = 0;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Interface = ParallelGlobal.SubDPs[i].Interface;
        SubDP->Name = ParallelGlobal.SubDPs[i].Name;
        SubDP->Scope = ParallelGlobal.SubDPs[i].Scope;
        if (SubDP->Interface->initWriter)
        {
            SubDP->WS_Stream =
                SubDP->Interface->initWriter(Svcs, CP_Stream, Params, DPAttrs, Stats);
        }
        if (SubDP->WS_Stream)
        {
            SubDP->Ready = 1;
            NumAvailable++;
        }
    }

    if (NumAvailable == 0)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Parallel DataPlane: none of the configured sub-dataplanes initialized "
                      "successfully on the writer side.\n");
        free(Stream->SubDPs);
        free(Stream->Hostname);
        free(Stream);
        return NULL;
    }
    return Stream;
}

static DP_WSR_Stream ParallelInitWriterPerReader(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                                 int ReaderCohortSize, CP_PeerCohort PeerCohort,
                                                 void **ProvidedReaderInfo,
                                                 void **WriterContactInfoPtr)
{
    Parallel_WS_Stream Stream = (Parallel_WS_Stream)WS_Stream_v;
    Parallel_WSR_Stream WSR = calloc(1, sizeof(*WSR));
    WSR->WS_Stream = Stream;
    WSR->NumSubDPs = Stream->NumSubDPs;
    WSR->SubWSR = calloc(Stream->NumSubDPs, sizeof(*WSR->SubWSR));

    void **ContactInfo = calloc(Stream->NumSubDPs, sizeof(*ContactInfo));
    int *Available = calloc(Stream->NumSubDPs, sizeof(*Available));

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (!SubDP->Ready || !SubDP->WS_Stream || !SubDP->Interface->initWriterPerReader)
        {
            continue;
        }

        int AllAvailable = 1;
        for (int j = 0; j < ReaderCohortSize; j++)
        {
            ParallelContactInfo ReaderContact = (ParallelContactInfo)ProvidedReaderInfo[j];
            int Slot = ReaderContact ? ParallelFindSlot(ReaderContact->SubInfo,
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
            ParallelContactInfo ReaderContact = (ParallelContactInfo)ProvidedReaderInfo[j];
            int Slot =
                ParallelFindSlot(ReaderContact->SubInfo, ReaderContact->NumSubDPs, SubDP->Name);
            ReaderInfo[j] =
                ParallelDecodeSubInfoReal(ParallelGlobal.FFS, &ReaderContact->SubInfo[Slot]);
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

    ParallelContactInfo Contact =
        ParallelMakeContact(ParallelGlobal.WriterContactFormats, ContactInfo, Available,
                            Stream->Hostname, (int)Stream->NumSubDPs);
    free(ContactInfo);
    free(Available);

    WSR->MyContact = Contact;
    *WriterContactInfoPtr = Contact;
    return WSR;
}

static void ParallelProvideWriterDataToReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                              int WriterCohortSize, CP_PeerCohort PeerCohort,
                                              void **ProvidedWriterInfo)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
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
        ParallelContactInfo WriterContact = (ParallelContactInfo)ProvidedWriterInfo[j];
        if (WriterContact && WriterContact->Hostname)
        {
            Stream->WriterHostnames[j] = strdup(WriterContact->Hostname);
        }
    }

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        SubDP->Ready = 0;
        if (!SubDP->RS_Stream || !SubDP->Interface->provideWriterDataToReader)
        {
            continue;
        }

        int AllAvailable = 1;
        for (int j = 0; j < WriterCohortSize; j++)
        {
            ParallelContactInfo WriterContact = (ParallelContactInfo)ProvidedWriterInfo[j];
            int Slot = WriterContact ? ParallelFindSlot(WriterContact->SubInfo,
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
            ParallelContactInfo WriterContact = (ParallelContactInfo)ProvidedWriterInfo[j];
            int Slot =
                ParallelFindSlot(WriterContact->SubInfo, WriterContact->NumSubDPs, SubDP->Name);
            WriterInfo[j] =
                ParallelDecodeSubInfoReal(ParallelGlobal.FFS, &WriterContact->SubInfo[Slot]);
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

static void *ParallelReadRemoteMemory(CP_Services Svcs, DP_RS_Stream RS_Stream_v, int Rank,
                                      size_t Timestep, size_t Offset, size_t Length, void *Buffer,
                                      void *DP_TimestepInfo)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
    ParallelTimestepInfo TimestepInfo = (ParallelTimestepInfo)DP_TimestepInfo;
    int SameHost = ParallelSameHost(Stream, Rank);

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        if (!ParallelSubDPIsCandidate(Stream, i, SameHost))
        {
            continue;
        }

        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        void *SubTimestepInfo = NULL;
        if (SubDP->Interface->TimestepInfoFormats)
        {
            int Slot = TimestepInfo ? ParallelFindSlot(TimestepInfo->SubInfo,
                                                       TimestepInfo->NumSubInfos, SubDP->Name)
                                    : -1;
            if (Slot < 0 || !TimestepInfo->SubInfo[Slot].Available)
            {
                continue;
            }
            SubTimestepInfo =
                ParallelDecodeSubInfoReal(ParallelGlobal.FFS, &TimestepInfo->SubInfo[Slot]);
            if (!SubTimestepInfo)
            {
                continue;
            }
        }

        DP_CompletionHandle Handle = SubDP->Interface->readRemoteMemory(
            Svcs, SubDP->RS_Stream, Rank, Timestep, Offset, Length, Buffer, SubTimestepInfo);
        free(SubTimestepInfo);
        if (Handle)
        {
            ParallelCompletionHandle Completion = malloc(sizeof(*Completion));
            Completion->Interface = SubDP->Interface;
            Completion->Handle = Handle;
            return Completion;
        }
    }
    return NULL;
}

static int ParallelWaitForCompletion(CP_Services Svcs, void *Handle_v)
{
    ParallelCompletionHandle Completion = (ParallelCompletionHandle)Handle_v;
    if (!Completion)
    {
        return 0;
    }
    int Ret = 1;
    if (Completion->Interface && Completion->Interface->waitForCompletion)
    {
        Ret = Completion->Interface->waitForCompletion(Svcs, Completion->Handle);
    }
    free(Completion);
    return Ret;
}

static void ParallelNotifyConnFailure(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                      int FailedPeerRank)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->notifyConnFailure)
        {
            SubDP->Interface->notifyConnFailure(Svcs, SubDP->RS_Stream, FailedPeerRank);
        }
    }
}

static void ParallelTimestepArrived(CP_Services Svcs, DP_RS_Stream RS_Stream_v, size_t Timestep,
                                    SstPreloadModeType PreloadMode)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->timestepArrived)
        {
            SubDP->Interface->timestepArrived(Svcs, SubDP->RS_Stream, Timestep, PreloadMode);
        }
    }
}

static void ParallelReaderRegisterTimestep(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                           size_t Timestep, SstPreloadModeType PreloadMode)
{
    Parallel_WSR_Stream WSR = (Parallel_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->readerRegisterTimestep)
        {
            WSR->WS_Stream->SubDPs[i].Interface->readerRegisterTimestep(Svcs, WSR->SubWSR[i],
                                                                        Timestep, PreloadMode);
        }
    }
}

static void ParallelReaderReleaseTimestep(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                          size_t Timestep)
{
    Parallel_WSR_Stream WSR = (Parallel_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->readerReleaseTimestep)
        {
            WSR->WS_Stream->SubDPs[i].Interface->readerReleaseTimestep(Svcs, WSR->SubWSR[i],
                                                                       Timestep);
        }
    }
}

static void ParallelWSRReadPatternLocked(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v,
                                         size_t EffectiveTimestep)
{
    Parallel_WSR_Stream WSR = (Parallel_WSR_Stream)WSR_Stream_v;
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        if (WSR->SubWSR[i] && WSR->WS_Stream->SubDPs[i].Interface->WSRreadPatternLocked)
        {
            WSR->WS_Stream->SubDPs[i].Interface->WSRreadPatternLocked(Svcs, WSR->SubWSR[i],
                                                                      EffectiveTimestep);
        }
    }
}

static void ParallelRSReadPatternLocked(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                        size_t EffectiveTimestep)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->RSreadPatternLocked)
        {
            SubDP->Interface->RSreadPatternLocked(Svcs, SubDP->RS_Stream, EffectiveTimestep);
        }
    }
}

static void ParallelRSReleaseTimestep(CP_Services Svcs, DP_RS_Stream RS_Stream_v, size_t Timestep)
{
    Parallel_RS_Stream Stream = (Parallel_RS_Stream)RS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->RS_Stream && SubDP->Interface->RSReleaseTimestep)
        {
            SubDP->Interface->RSReleaseTimestep(Svcs, SubDP->RS_Stream, Timestep);
        }
    }
}

static void ParallelProvideTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                    struct _SstData *Data, struct _SstData *LocalMetadata,
                                    size_t Timestep, void **TimestepInfoPtr)
{
    Parallel_WS_Stream Stream = (Parallel_WS_Stream)WS_Stream_v;
    ParallelTimestepInfo Info = calloc(1, sizeof(*Info));
    Info->NumSubInfos = (int)Stream->NumSubDPs;
    Info->SubInfo = calloc(Stream->NumSubDPs, sizeof(struct _ParallelSubDPInfo));

    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        Info->SubInfo[i].Name = strdup(ParallelGlobal.SubDPs[i].Name);
        if (!SubDP->Ready || !SubDP->WS_Stream || !SubDP->Interface->provideTimestep)
        {
            continue;
        }
        void *SubInfo = NULL;
        SubDP->Interface->provideTimestep(Svcs, SubDP->WS_Stream, Data, LocalMetadata, Timestep,
                                          &SubInfo);
        ParallelEncodeSubInfo(ParallelGlobal.TimestepInfoFormats[i], SubInfo, &Info->SubInfo[i]);
        if (SubDP->Interface->TimestepInfoFormats && Info->SubInfo[i].Length > 0)
        {
            Info->SubInfo[i].Available = 1;
        }
        else if (!SubDP->Interface->TimestepInfoFormats)
        {
            Info->SubInfo[i].Available = 1;
        }
    }

    ParallelTimestepEntry Entry = calloc(1, sizeof(*Entry));
    Entry->Timestep = Timestep;
    Entry->Info = Info;
    Entry->Next = Stream->Timesteps;
    Stream->Timesteps = Entry;

    *TimestepInfoPtr = Info;
}

static void ParallelReleaseTimestep(CP_Services Svcs, DP_WS_Stream WS_Stream_v, size_t Timestep)
{
    Parallel_WS_Stream Stream = (Parallel_WS_Stream)WS_Stream_v;
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->Ready && SubDP->WS_Stream && SubDP->Interface->releaseTimestep)
        {
            SubDP->Interface->releaseTimestep(Svcs, SubDP->WS_Stream, Timestep);
        }
    }

    ParallelTimestepEntry *Link = &Stream->Timesteps;
    while (*Link)
    {
        if ((*Link)->Timestep == Timestep)
        {
            ParallelTimestepEntry Entry = *Link;
            *Link = Entry->Next;
            ParallelFreeTimestepInfo(Entry->Info);
            free(Entry);
            break;
        }
        Link = &(*Link)->Next;
    }
}

static void ParallelDestroyWriterPerReader(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v)
{
    Parallel_WSR_Stream WSR = (Parallel_WSR_Stream)WSR_Stream_v;
    if (!WSR)
    {
        return;
    }
    for (size_t i = 0; i < WSR->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &WSR->WS_Stream->SubDPs[i];
        if (WSR->SubWSR[i] && SubDP->Interface->destroyWriterPerReader)
        {
            SubDP->Interface->destroyWriterPerReader(Svcs, WSR->SubWSR[i]);
        }
    }
    free(WSR->SubWSR);
    ParallelFreeContact(WSR->MyContact);
    free(WSR);
}

static void ParallelDestroyWriter(CP_Services Svcs, DP_WS_Stream WS_Stream_v)
{
    Parallel_WS_Stream Stream = (Parallel_WS_Stream)WS_Stream_v;
    if (!Stream)
    {
        return;
    }
    for (size_t i = 0; i < Stream->NumSubDPs; i++)
    {
        ParallelSubDPState SubDP = &Stream->SubDPs[i];
        if (SubDP->WS_Stream && SubDP->Interface->destroyWriter)
        {
            SubDP->Interface->destroyWriter(Svcs, SubDP->WS_Stream);
        }
    }
    ParallelTimestepEntry Entry = Stream->Timesteps;
    while (Entry)
    {
        ParallelTimestepEntry Next = Entry->Next;
        ParallelFreeTimestepInfo(Entry->Info);
        free(Entry);
        Entry = Next;
    }
    free(Stream->SubDPs);
    free(Stream->Hostname);
    free(Stream);
}

static int ParallelGetPriority(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params)
{
    ParallelEnsureConfig(Svcs, CP_Stream);
    if (ParallelGlobal.NumSubDPs == 0)
    {
        return -1;
    }

    int NumUsable = 0;
    for (int i = 0; i < ParallelGlobal.NumSubDPs; i++)
    {
        CP_DP_Interface Interface = ParallelGlobal.SubDPs[i].Interface;
        if (!Interface->getPriority || Interface->getPriority(Svcs, CP_Stream, Params) >= 0)
        {
            NumUsable++;
        }
    }
    if (NumUsable == 0)
    {
        Svcs->verbose(CP_Stream, DPCriticalVerbose,
                      "Parallel DataPlane: none of the configured sub-dataplanes is usable.\n");
        return -1;
    }
    return 1000;
}

static void ParallelUnGetPriority(CP_Services Svcs, void *CP_Stream)
{
    Svcs->verbose(CP_Stream, DPPerStepVerbose, "Parallel Dataplane unloading\n");
}

static struct _CP_DP_Interface ParallelDPInterface = {0};

extern CP_DP_Interface LoadParallelDP()
{
    ParallelDPInterface.DPName = "parallel";
    ParallelDPInterface.ReaderContactFormats = ParallelReaderContactStructs;
    ParallelDPInterface.WriterContactFormats = ParallelWriterContactStructs;
    ParallelDPInterface.TimestepInfoFormats = ParallelTimestepInfoStructs;
    ParallelDPInterface.initReader = ParallelInitReader;
    ParallelDPInterface.initWriter = ParallelInitWriter;
    ParallelDPInterface.initWriterPerReader = ParallelInitWriterPerReader;
    ParallelDPInterface.provideWriterDataToReader = ParallelProvideWriterDataToReader;
    ParallelDPInterface.readRemoteMemory = (CP_DP_ReadRemoteMemoryFunc)ParallelReadRemoteMemory;
    ParallelDPInterface.waitForCompletion = ParallelWaitForCompletion;
    ParallelDPInterface.notifyConnFailure = ParallelNotifyConnFailure;
    ParallelDPInterface.provideTimestep = (CP_DP_ProvideTimestepFunc)ParallelProvideTimestep;
    ParallelDPInterface.readerRegisterTimestep = ParallelReaderRegisterTimestep;
    ParallelDPInterface.releaseTimestep = (CP_DP_ReleaseTimestepFunc)ParallelReleaseTimestep;
    ParallelDPInterface.readerReleaseTimestep = ParallelReaderReleaseTimestep;
    ParallelDPInterface.WSRreadPatternLocked = ParallelWSRReadPatternLocked;
    ParallelDPInterface.RSreadPatternLocked = ParallelRSReadPatternLocked;
    ParallelDPInterface.RSReleaseTimestep = ParallelRSReleaseTimestep;
    ParallelDPInterface.timestepArrived = (CP_DP_ReaderTimestepArrivalFunc)ParallelTimestepArrived;
    ParallelDPInterface.destroyReader = ParallelDestroyReader;
    ParallelDPInterface.destroyWriter = ParallelDestroyWriter;
    ParallelDPInterface.destroyWriterPerReader = ParallelDestroyWriterPerReader;
    ParallelDPInterface.getPriority = ParallelGetPriority;
    ParallelDPInterface.unGetPriority = ParallelUnGetPriority;
    return &ParallelDPInterface;
}
