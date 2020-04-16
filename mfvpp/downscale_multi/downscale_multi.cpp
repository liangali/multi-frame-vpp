
#include "cm_rt.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <stdio.h>
#include <fstream>

using namespace std;

#define QUEUE_NUM  4
#define KERNEL_NUM 1

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

char* gKernelNameList[2] = { "downscale16", "downscale32" };

struct CmdOption
{
    int srcW;
    int srcH;
    int dstW;
    int dstH;
    int runNum;
    char* infileName;
    int kIndex = 0;
};

struct ImgData {
    int w;
    int h;
    int size;
    char* buf;
    
    ImgData(int width, int height)
    {
        w = width;
        h = height;
        size = w * h * 3 / 2;
        buf = (char*)_aligned_malloc(size, 0x1000);
        memset(buf, 0, size);
    }

    int readNV12(char* filename)
    {
        ifstream infile(filename, ios::binary);
        if (!infile.is_open()) {
            printf("ERROR: cannot open input file %s\n", filename);
            return -1;
        }
        infile.read((char*)buf, size);
        infile.close();
        return 0;
    }

    void dumpNV12(char* filename)
    {
        ofstream of(filename, ios::binary);
        of.write(buf, size);
        of.close();
    }

    ~ImgData() 
    {
        if (buf) {
            _aligned_free(buf);
            buf = nullptr;
        }
    }
};

struct KernelContext
{
    CmKernel* kernel;
    CmSurface2D* srcSurf;
    CmSurface2D* dstSurf;
    SurfaceIndex* srcIdx;
    SurfaceIndex* dstIdx;
    CmSampler* sampler;
    SamplerIndex* samplerIdx;
    CmEvent* event;
};

struct QueueContext
{
    CmQueue* queue;
    CmTask* task;
    CmThreadSpace* ts;
    CmThreadGroupSpace* tgs;
    KernelContext kctx[KERNEL_NUM];
};

struct CmContext
{
    CmDevice* pCmDev = NULL;
    CmProgram* pProgram = NULL;
    void* pCommonISACode = NULL;
    QueueContext queueCtx[QUEUE_NUM] = {};
};

int cmdOpt(int argc, char** argv, CmdOption& cmd)
{
    if (argc == 1) {
        cmd.srcW = 1920;
        cmd.srcH = 1080;
        cmd.dstW = 300;
        cmd.dstH = 300;
        cmd.infileName = "test.nv12";
        cmd.kIndex = 0;
        cmd.runNum = 1;
    } else if (argc == 5 || argc == 6 || argc == 7) {
        cmd.srcW = atoi(argv[1]);
        cmd.srcH = atoi(argv[2]);
        cmd.dstW = atoi(argv[3]);
        cmd.dstH = atoi(argv[4]);
        cmd.infileName = argv[5];
        if (argc == 5) {
            cmd.kIndex = 0;
            cmd.runNum = 100;
        } 
        else if (argc == 6) {
            cmd.kIndex = (atoi(argv[6]) == 0) ? 0 : 1;
            cmd.runNum = 100;
        }
        else {
            cmd.kIndex = (atoi(argv[6]) == 0) ? 0 : 1;
            cmd.runNum = atoi(argv[7]);
        }
    } else {
        printf("ERROR: invalid cmd line!\n");
        return -1;
    }
    return 0;
}

int initKernel(CmContext& ctx, KernelContext* kctx, CmTask* task, int kIndex, const ImgData* srcImg, const ImgData* dstImg)
{
    int cmRet = 0;
    cmRet = ctx.pCmDev->CreateKernel(ctx.pProgram, gKernelNameList[kIndex], kctx->kernel);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateKernel error\n");
        return -1;
    }

    // 3D Sampler
    CM_SAMPLER_STATE  sampleState = {};
    sampleState.magFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.minFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.addressU = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressV = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressW = CM_TEXTURE_ADDRESS_CLAMP;
    cmRet = ctx.pCmDev->CreateSampler(sampleState, kctx->sampler);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSampler error\n");
        return -1;
    }
    kctx->sampler->GetIndex(kctx->samplerIdx);

    // Source surface
    unsigned int pitch, physicalSize;
    if (((unsigned long long)srcImg->buf & 0xfff) == 0) {
        ctx.pCmDev->GetSurface2DInfo(srcImg->w, srcImg->h, CM_SURFACE_FORMAT_NV12, pitch, physicalSize);
        printf("INFO: CmSurface2D w = %d, h = %d, pitch = %d, size = %d\n", srcImg->w, srcImg->h, pitch, physicalSize);
    } else {
        printf("ERROR: Src buf is not 4K aligned\n");
        return -1;
    }
    cmRet = ctx.pCmDev->CreateSurface2D(srcImg->w, srcImg->h, CM_SURFACE_FORMAT_NV12, kctx->srcSurf);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSurface2D error\n");
        return -1;
    }
    cmRet = kctx->srcSurf->WriteSurface((unsigned char*)srcImg->buf, NULL);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM WriteSurface error\n");
        return -1;
    }
    cmRet = ctx.pCmDev->CreateSamplerSurface2D(kctx->srcSurf, kctx->srcIdx);
    if (cmRet != CM_SUCCESS) {
        printf("CM CreateSamplerUPSurface error");
        return -1;
    }

    // Destination surface
    cmRet = ctx.pCmDev->CreateSurface2D(dstImg->w, dstImg->h, CM_SURFACE_FORMAT_NV12, kctx->dstSurf);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSurface2D error\n");
        return -1;
    }
    kctx->dstSurf->GetIndex(kctx->dstIdx);

    CmKernel* kernel = kctx->kernel;
    int threadWidth = (dstImg->w + DS_THREAD_WIDTH - 1) / DS_THREAD_WIDTH;
    int threadHeight = (dstImg->h + DS_THREAD_HEIGHT - 1) / DS_THREAD_HEIGHT;
    kernel->SetThreadCount(threadWidth * threadHeight);
    printf("INFO: HW thread_w = %d, thread_h = %d, total = %d\n", 
        threadWidth, threadHeight, threadWidth * threadHeight);

    kernel->SetKernelArg(0, sizeof(SurfaceIndex), kctx->srcIdx);
    kernel->SetKernelArg(1, sizeof(SamplerIndex), kctx->samplerIdx);
    kernel->SetKernelArg(2, sizeof(SurfaceIndex), kctx->dstIdx);
    kernel->SetKernelArg(3, sizeof(unsigned short), &dstImg->w);
    kernel->SetKernelArg(4, sizeof(unsigned short), &dstImg->h);

    cmRet = task->AddKernel(kernel);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CmDevice AddKernel error\n");
        return -1;
    }

    return 0;
}

int initQueue(CmContext& ctx, const CmdOption* cmd, const ImgData* srcImg, const ImgData* dstImg)
{
    int cmRet = 0;
    for (int i=0; i<QUEUE_NUM; i++) {
        cmRet = ctx.pCmDev->CreateQueue(ctx.queueCtx[i].queue);
        if (cmRet != CM_SUCCESS) {
            printf("ERROR: CM CreateQueue error\n");
            return -1;
        }

        cmRet = ctx.pCmDev->CreateTask(ctx.queueCtx[i].task);
        if (cmRet != CM_SUCCESS) {
            printf("ERROR: CmDevice CreateTask error\n");
            return -1;
        }

        int threadWidth = (dstImg->w + DS_THREAD_WIDTH - 1) / DS_THREAD_WIDTH;
        int threadHeight = (dstImg->h + DS_THREAD_HEIGHT - 1) / DS_THREAD_HEIGHT;
        cmRet = ctx.pCmDev->CreateThreadSpace(threadWidth, threadHeight, ctx.queueCtx[i].ts);
        if (cmRet != CM_SUCCESS) {
            printf("ERROR: CM CreateThreadSpace error\n");
            return -1;
        }

        for (int j=0; j<KERNEL_NUM; j++) {
            if (initKernel(ctx, &ctx.queueCtx[i].kctx[j], ctx.queueCtx[i].task, cmd->kIndex, srcImg, dstImg)) {
                return -1;
            }
        }
    }

    return 0;
}

int initCM(CmContext& ctx, const CmdOption* cmd, const ImgData* srcImg, const ImgData* dstImg)
{
    int cmRet = 0;
    unsigned int version = 0;

    cmRet = ::CreateCmDevice(ctx.pCmDev, version);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CmDevice creation error");
        return -1;
    }
    if (version < CM_1_0) {
        printf("ERROR: The runtime API version is later than runtime DLL version");
        return -1;
    }

    FILE* pFileISA = nullptr;
    fopen_s(&pFileISA, "downscale_multi_genx.isa", "rb");
    if (pFileISA == NULL) {
        printf("ERROR: failed to open downscale_multi_genx.isa\n");
        return -1;
    }
    fseek(pFileISA, 0, SEEK_END);
    int codeSize = ftell(pFileISA);
    rewind(pFileISA);
    if (codeSize == 0) {
        printf("ERROR: invalid ISA file\n");
        return -1;
    }
    ctx.pCommonISACode = (BYTE*)malloc(codeSize);
    if (!ctx.pCommonISACode) {
        printf("ERROR: failed to allocate memory for ISA code, size = %d\n", codeSize);
        return -1;
    }
    if (fread(ctx.pCommonISACode, 1, codeSize, pFileISA) != codeSize) {
        printf("ERROR: failed to read ISA file\n");
        return -1;
    }
    fclose(pFileISA);

    cmRet = ctx.pCmDev->LoadProgram(ctx.pCommonISACode, codeSize, ctx.pProgram);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM LoadProgram error\n");
        return -1;
    }

    if (initQueue(ctx, cmd, srcImg, dstImg)) {
        return -1;
    }

    return 0;
}

void destroyCM(CmContext& ctx)
{
    if (ctx.pCommonISACode) {
        free(ctx.pCommonISACode);
    }

    for (int i=0; i<QUEUE_NUM; i++) {
        ctx.pCmDev->DestroyTask(ctx.queueCtx[i].task);
        ctx.pCmDev->DestroyThreadSpace(ctx.queueCtx[i].ts);
        for (int j=0; j<KERNEL_NUM; j++) {
            ctx.pCmDev->DestroyKernel(ctx.queueCtx[i].kctx[j].kernel);
            ctx.pCmDev->DestroySurface(ctx.queueCtx[i].kctx[j].srcSurf);
            ctx.pCmDev->DestroySurface(ctx.queueCtx[i].kctx[j].dstSurf);
            ctx.pCmDev->DestroySampler(ctx.queueCtx[i].kctx[j].sampler);
        }
    }

    ctx.pCmDev->DestroyProgram(ctx.pProgram);
    ::DestroyCmDevice(ctx.pCmDev);
}

int main(int argc, char* argv[])
{
    CmdOption cmd = {};
    if (cmdOpt(argc, argv,  cmd)) {
        return -1;
    }

    ImgData srcImg(cmd.srcW, cmd.srcH);
    ImgData dstImg(cmd.dstW, cmd.dstH);
    if (srcImg.readNV12(cmd.infileName)) {
        printf("ERROR: initialize src img failed \n");
        return -1;
    }

    CmContext cmCtx = {};
    if (initCM(cmCtx, &cmd, &srcImg, &dstImg)) {
        return -1;
    }

    int cmRet = 0;
    double totalTime = 0.0;
    for (size_t i = 0; i < cmd.runNum; i++) {
        for (int j=0; j<QUEUE_NUM; j++) {
            CmQueue* pCmQueue = cmCtx.queueCtx[j].queue;
            CmTask* task = cmCtx.queueCtx[j].task;
            CmThreadSpace* ts = cmCtx.queueCtx[j].ts;
            CmEvent* e = cmCtx.queueCtx[j].kctx[0].event;
            CmSurface2D* pDstSurface = cmCtx.queueCtx[j].kctx[0].dstSurf;

            cmRet = pCmQueue->Enqueue(task, e, ts);
            if (cmRet != CM_SUCCESS) {
                printf("ERROR: CmDevice enqueue error\n");
                return -1;
            }

            cmRet = pDstSurface->ReadSurface((unsigned char*)dstImg.buf, e);
            if (cmRet != CM_SUCCESS) {
                printf("ERROR: CM ReadSurface error\n");
                return -1;
            }

            static int dumped = 0;
            if (!dumped) {
                dumped = 1;
                dstImg.dumpNV12("out.nv12");
            }

            UINT64 executionTime = 0;
            cmRet = e->GetExecutionTime(executionTime);
            if (cmRet != CM_SUCCESS) {
                printf("CM GetExecutionTime error");
                return -1;
            }
            totalTime += executionTime;
        }
    }
    printf("INFO: Average execution time: %f us; enqueue_times = %d\n", totalTime / (cmd.runNum*QUEUE_NUM) / 1000.0, cmd.runNum*QUEUE_NUM);

    destroyCM(cmCtx);

    printf("done!\n");
    return 0;
}
