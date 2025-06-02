#include "dp_interface.h"

#include <atl.h>
#include <evpath.h>
#include <stdio.h>

#include "sys/types.h"

struct fabric_state
{
    struct _CP_DP_Interface *list_dps;
    size_t num_dps;
};

typedef struct _Parallel_WS_Stream
{
    DP_WS_Stream *list_substreams;
    size_t num_substreams;
} Parallel_WS_Stream;

static struct _CP_DP_Interface ParallelDPInterface = {0};

typedef struct _ParallelReaderContactInfo
{
    void *RS_Stream;
} *ParallelReaderContactInfo;

static FMField ParallelReaderContactList[] = {
    {"reader_ID", "integer", sizeof(void *), FMOffset(ParallelReaderContactInfo, RS_Stream)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelReaderContactStructs[] = {
    {"ParallelReaderContactInfo", ParallelReaderContactList,
     sizeof(struct _ParallelReaderContactInfo), NULL},
    {NULL, NULL, 0, NULL}};

typedef struct _ParallelWriterContactInfo
{
    void *WS_Stream;
    size_t Length;
    void *Address;
} *ParallelWriterContactInfo;

static FMField ParallelWriterContactList[] = {
    {"writer_ID", "integer", sizeof(void *), FMOffset(ParallelWriterContactInfo, WS_Stream)},
    {"Length", "integer", sizeof(int), FMOffset(ParallelWriterContactInfo, Length)},
    {"Address", "integer[Length]", sizeof(char), FMOffset(ParallelWriterContactInfo, Address)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelWriterContactStructs[] = {
    {"ParallelWriterContactInfo", ParallelWriterContactList,
     sizeof(struct _ParallelWriterContactInfo), NULL},
    {NULL, NULL, 0, NULL}};

typedef struct _ParallelBufferHandle
{
    void *Block;
    size_t rkey_size;
    char *rkey;
} *ParallelBufferHandle;

static FMField ParallelBufferHandleList[] = {
    {"Block", "integer", sizeof(void *), FMOffset(ParallelBufferHandle, Block)},
    {"rkey_size", "integer", sizeof(size_t), FMOffset(ParallelBufferHandle, rkey_size)},
    {"rkey", "char[rkey_size]", sizeof(char), FMOffset(ParallelBufferHandle, rkey)},
    {NULL, NULL, 0, 0}};

static FMStructDescRec ParallelBufferHandleStructs[] = {
    {"ParallelBufferHandle", ParallelBufferHandleList, sizeof(struct _ParallelBufferHandle), NULL},
    {NULL, NULL, 0, NULL}};

///////////////////////////////////////////////

static DP_RS_Stream ParallelInitReader(CP_Services Svcs, void *CP_Stream,
                                       void **ReaderContactInfoPtr, struct _SstParams *Params,
                                       attr_list WriterContact, SstStats Stats)
{
    printf("InitReader\n");
    return NULL;
}

static DP_WS_Stream ParallelInitWriter(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params,
                                       attr_list DPAttrs, SstStats Stats)
{
    printf("InitWriter\n");
    return NULL;
}

static DP_WSR_Stream ParallelInitWriterPerReader(CP_Services Svcs, DP_WS_Stream WS_Stream_v,
                                                 int readerCohortSize, CP_PeerCohort PeerCohort,
                                                 void **providedReaderInfo_v,
                                                 void **WriterContactInfoPtr)
{
    printf("InitWriterPerReader\n");
    return NULL;
}

static void ParallelProvideWriterDataToReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v,
                                              int writerCohortSize, CP_PeerCohort PeerCohort,
                                              void **providedWriterInfo_v)
{
    printf("ProvideWriterDataToReader\n");
    return;
}

static void *ParallelReadRemoteMemory(CP_Services Svcs, DP_RS_Stream Stream_v, int Rank,
                                      long Timestep, size_t Offset, size_t Length, void *Buffer,
                                      void *DP_TimestepInfo)
{
    printf("ReadRemoteMemory\n");
    return NULL;
}

static int ParallelWaitForCompletion(CP_Services Svcs, void *Handle_v)
{
    printf("WaitForCompletion\n");
    return 0;
}

static void ParallelProvideTimestep(CP_Services Svcs, DP_WS_Stream Stream_v, struct _SstData *Data,
                                    struct _SstData *LocalMetadata, long Timestep,
                                    void **TimestepInfoPtr)
{
    printf("ProvideTimestep\n");
    return;
}

static void ParallelReleaseTimestep(CP_Services Svcs, DP_WS_Stream Stream_v, long Timestep)
{
    printf("ReleaseTimestep\n");
    return;
}

static void ParallelDestroyReader(CP_Services Svcs, DP_RS_Stream RS_Stream_v)
{
    printf("DestroyReader\n");
    return;
}

static void ParallelDestroyWriterPerReader(CP_Services Svcs, DP_WSR_Stream WSR_Stream_v)
{
    printf("DestroyWriterPerReader\n");
    return;
}

static void ParallelNotifyConnFailure(CP_Services Svcs, DP_RS_Stream Stream_v, int FailedPeerRank)
{
    printf("NotifyConnFailure\n");
    return;
}

static void ParallelTimestepArrived(CP_Services Svcs, DP_RS_Stream Stream_v, long Timestep,
                                    SstPreloadModeType PreloadMode)
{
    printf("TimestepArrived\n");
    return;
}

static void ParallelDestroyWriter(CP_Services Svcs, DP_WS_Stream WS_Stream_v)
{
    printf("DestroyWriter\n");
    return;
}

static int ParallelGetPriority(CP_Services Svcs, void *CP_Stream, struct _SstParams *Params)
{
    /* TODO: Use environment variable such as: SST_PARALLEL_DP=rdma to decide this
       For now, hardcoded. */
    int ux_dp_priority = 1000;

    return ux_dp_priority;
}

static void ParallelUnGetPriority(CP_Services Svcs, void *CP_Stream)
{
    Svcs->verbose(CP_Stream, DPPerStepVerbose, "Parallel Dataplane unloading\n");
}

extern CP_DP_Interface LoadParallelDP()
{
    ParallelDPInterface.DPName = "parallel";
    ParallelDPInterface.ReaderContactFormats = ParallelReaderContactStructs;
    ParallelDPInterface.WriterContactFormats = ParallelWriterContactStructs;
    ParallelDPInterface.TimestepInfoFormats = ParallelBufferHandleStructs;
    ParallelDPInterface.initReader = ParallelInitReader;
    ParallelDPInterface.initWriter = ParallelInitWriter;
    ParallelDPInterface.initWriterPerReader = ParallelInitWriterPerReader;
    ParallelDPInterface.provideWriterDataToReader = ParallelProvideWriterDataToReader;
    ParallelDPInterface.readRemoteMemory = (CP_DP_ReadRemoteMemoryFunc)ParallelReadRemoteMemory;
    ParallelDPInterface.waitForCompletion = ParallelWaitForCompletion;
    ParallelDPInterface.notifyConnFailure = ParallelNotifyConnFailure;
    ParallelDPInterface.provideTimestep = (CP_DP_ProvideTimestepFunc)ParallelProvideTimestep;
    ParallelDPInterface.readerRegisterTimestep = NULL;
    ParallelDPInterface.releaseTimestep = (CP_DP_ReleaseTimestepFunc)ParallelReleaseTimestep;
    ParallelDPInterface.readerReleaseTimestep = NULL;
    ParallelDPInterface.WSRreadPatternLocked = NULL;
    ParallelDPInterface.RSreadPatternLocked = NULL;
    ParallelDPInterface.RSReleaseTimestep = NULL;
    ParallelDPInterface.timestepArrived = (CP_DP_ReaderTimestepArrivalFunc)ParallelTimestepArrived;
    ParallelDPInterface.destroyReader = ParallelDestroyReader;
    ParallelDPInterface.destroyWriter = ParallelDestroyWriter;
    ParallelDPInterface.destroyWriterPerReader = ParallelDestroyWriterPerReader;
    ParallelDPInterface.getPriority = ParallelGetPriority;
    ParallelDPInterface.unGetPriority = ParallelUnGetPriority;
    return &ParallelDPInterface;
}
