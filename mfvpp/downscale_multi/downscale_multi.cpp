
#include "cm_rt.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <stdio.h>
#include <fstream>

#ifdef LINUX
#include <stdlib.h>
#include <va/va.h>
#include "../display.cpp.inc"
#endif

using namespace std;

#define QUEUE_NUM  4
#define KERNEL_NUM 1

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

char* gKernelNameList[2] = { "downscale16", "downscale32" };
#ifdef LINUX
static VADisplay va_dpy = nullptr;
extern VADisplay display;
#endif

struct CmdOption
{
    int srcW;           // argv[1]
    int srcH;           // argv[2]
    int dstW;           // argv[3]
    int dstH;           // argv[4]
    char* infileName;   // argv[5]
    int kIndex = 0;     // argv[6]
    int ccsNum;         // argv[7]
    int kernelNum;      // argv[8]
    int runNum;         // argv[9]

    void helper()
    {
        printf("\nCommand line usage: \n");
        printf("downscale_multi.exe srcW srcH dstW dstH input.nv12 sample16/32[0/1] ccs_count[0/1/2/3/4] kernel_per_ccs[1~8] run_num\n");
        printf("Usage examples: \n");
        printf("    downscale_multi.exe \n");
        printf("    downscale_multi.exe 1920 1080 300 300 test.nv12\n");
        printf("    downscale_multi.exe 1920 1080 300 300 test.nv12 0 0 1 200\n");
    };
    void print()
    {
        printf("\nCurrent commandline arguments: \n");
        printf("    srcW = %d\n", srcW);
        printf("    srcH = %d\n", srcH);
        printf("    dstW = %d\n", dstW);
        printf("    dstH = %d\n", dstH);
        printf("    infileName = %s\n", infileName);
        printf("    kIndex = %s\n", kIndex == 0 ? ("sample16") : ("sample32"));
        printf("    ccsNum = %s:%d\n", ccsNum == 0 ? ("RCS") : ("CCS"), ccsNum == 0 ? 1: ccsNum);
        printf("    kernelNum = %d\n", kernelNum);
        printf("    runNum = %d\n", runNum);
        printf("\n");
    };
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
#ifdef LINUX
        buf = (char*)memalign(0x1000, size);
#else
        buf = (char*)_aligned_malloc(size, 0x1000);   
#endif
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
#ifdef LINUX
            free(buf);
#else
            _aligned_free(buf);
#endif
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
    CmQueue *queue;
    CmTask *task;
    CmThreadGroupSpace* groupSpace;
    KernelContext kctx[KERNEL_NUM];
};

struct CmContext
{
    CmDevice* pCmDev = nullptr;
    CmProgram* pProgram = nullptr;
    void* pCommonISACode = nullptr;
    QueueContext queueCtx[QUEUE_NUM] = {};
};

int cmdOpt(int argc, char** argv, CmdOption& cmd)
{
    cmd.helper();
    if (argc == 1) {
        cmd.srcW = 1920;
        cmd.srcH = 1080;
        cmd.dstW = 300;
        cmd.dstH = 300;
        cmd.infileName = "test.nv12";
        cmd.kIndex = 0; // sample16
        cmd.ccsNum = 0; // RCS
        cmd.kernelNum = 1; // 1 kernel per CCS/RCS
        cmd.runNum = 100;
    } else if (argc == 6 || argc == 10) {
        cmd.srcW = atoi(argv[1]);
        cmd.srcH = atoi(argv[2]);
        cmd.dstW = atoi(argv[3]);
        cmd.dstH = atoi(argv[4]);
        cmd.infileName = argv[5];
        if (argc == 6) {
            cmd.kIndex = 0; // sample16
            cmd.ccsNum = 0; // RCS
            cmd.kernelNum = 1; // 1 kernel per CCS/RCS
            cmd.runNum = 100;
        } else {
            cmd.kIndex = (atoi(argv[6]) == 0) ? 0 : 1;
            cmd.ccsNum = atoi(argv[7]);
            cmd.kernelNum = atoi(argv[8]);
            cmd.runNum = atoi(argv[9]);
        }
    } else {
        printf("ERROR: invalid cmd line!\n");
        return -1;
    }

    cmd.print();
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
    cmRet = kctx->srcSurf->WriteSurface((unsigned char*)srcImg->buf, nullptr);
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
    CM_QUEUE_CREATE_OPTION option = {};
    option.QueueType = CM_QUEUE_TYPE_RENDER; // CM_QUEUE_TYPE_RENDER, CM_QUEUE_TYPE_COMPUTE
    for (int i=0; i<QUEUE_NUM; i++) {
        cmRet = ctx.pCmDev->CreateQueueEx(ctx.queueCtx[i].queue, option);
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
        cmRet = ctx.pCmDev->CreateThreadGroupSpace(1, 1, threadWidth, threadHeight,
                                                   ctx.queueCtx[i].groupSpace);
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

int initCM(CmContext& ctx, const CmdOption* cmd)
{
    int cmRet = 0;
    unsigned int version = 0;

#ifdef LINUX
    cmRet = ::CreateCmDevice(ctx.pCmDev, version, va::display);
#else
    cmRet = ::CreateCmDevice(ctx.pCmDev, version);
#endif
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CmDevice creation error");
        return -1;
    }
    if (version < CM_1_0) {
        printf("ERROR: The runtime API version is later than runtime DLL version");
        return -1;
    }

    FILE* pFileISA = nullptr;
#ifdef LINUX
    pFileISA = fopen("downscale_multi_genx.isa", "rb");
#else
    fopen_s(&pFileISA, "downscale_multi_genx.isa", "rb");
#endif
    if (pFileISA == nullptr) {
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

    return 0;
}

void destroyCM(CmContext& ctx)
{
    if (ctx.pCommonISACode) {
        free(ctx.pCommonISACode);
    }

    for (int i = 0; i < QUEUE_NUM; i++) {
        ctx.pCmDev->DestroyTask(ctx.queueCtx[i].task);
        ctx.pCmDev->DestroyThreadGroupSpace(ctx.queueCtx[i].groupSpace);
        for (int j = 0; j < KERNEL_NUM; j++) {
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

    printf("run mfvp under %d ccs mode.\n", QUEUE_NUM);

    ImgData srcImg(cmd.srcW, cmd.srcH);
    ImgData dstImg(cmd.dstW, cmd.dstH);
    if (srcImg.readNV12(cmd.infileName)) {
        printf("ERROR: initialize src img failed \n");
        return -1;
    }

#ifdef LINUX
    if (va::display == nullptr)
    {
        if (!va::openDisplay())
        {
            std::cout<<"KCFGPU: initMDF failed due to m_va_dpy = nullptr"<< std::endl;
            return -1;
        }
    }
#endif

    CmContext cmCtx = {};
    if (initCM(cmCtx, &cmd)) {
        return -1;
    }
    if (initQueue(cmCtx, &cmd, &srcImg, &dstImg)) {
        return -1;
    }

    int cmRet = 0;
    double totalTime = 0.0;
    for (size_t i = 0; i < cmd.runNum; i++) {
        for (int j=0; j<QUEUE_NUM; j++) {
            CmQueue* pCmQueue = cmCtx.queueCtx[j].queue;
            CmTask* task = cmCtx.queueCtx[j].task;
            CmThreadGroupSpace* groupSpace = cmCtx.queueCtx[j].groupSpace;
            CmEvent* e = cmCtx.queueCtx[j].kctx[0].event;
            CmSurface2D* pDstSurface = cmCtx.queueCtx[j].kctx[0].dstSurf;

            cmRet = pCmQueue->EnqueueWithGroup(task, e, groupSpace);
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
    printf("INFO: Average execution time: %f us; enqueue_times = %d\n", 
        totalTime / (cmd.runNum*QUEUE_NUM) / 1000.0, cmd.runNum*QUEUE_NUM);

    destroyCM(cmCtx);

#ifdef LINUX
    if (va::display != nullptr)
        va::closeDisplay();
#endif
    printf("done!\n");
    return 0;
}
