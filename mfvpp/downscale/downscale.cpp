
#include "cm_rt.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <stdio.h>
#include <fstream>
#ifdef LINUX
#include "../display.cpp.inc"
#include <va/va.h>
#endif

using namespace std;

char* gKernelNameList[2] = {"downscale16", "downscale32"};

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

struct ImgData {
    int w;
    int h;
    int size;
    char* buf;
};

int srcW, srcH, dstW, dstH;
ImgData img = {};
int kindex = 0;
int runNum = 100;
int cmRet = 0;
CmDevice* pCmDev = NULL;
CmProgram* pProgram = NULL;
void* pCommonISACode = NULL;
#ifdef LINUX
static VADisplay display;
#endif

void readNV12(char*filename, ImgData &img)
{
    ifstream infile(filename, ios::binary);
    img.buf = new char[img.size];
    memset(img.buf, 0, img.size);
    infile.read(img.buf, img.size);
    infile.close();
}

void dumpNV12(char* filename, char* buf, int size)
{
    ofstream of(filename, ios::binary);
    of.write(buf, size);
    of.close();
}

int cmdOpt(int argc, char** argv)
{
    if (argc == 1) {
        srcW = 1920;
        srcH = 1080;
        dstW = 300;
        dstH = 300;
        img.w = srcW;
        img.h = srcH;
        img.size = srcW * srcH * 3 / 2;
        readNV12("test.nv12", img);
    } else if (argc == 6 || argc == 7) {
        srcW = atoi(argv[1]);
        srcH = atoi(argv[2]);
        dstW = atoi(argv[3]);
        dstH = atoi(argv[4]);
        img.w = srcW;
        img.h = srcH;
        img.size = srcW * srcH * 3 / 2;
        readNV12(argv[5], img);
        kindex = (argc == 7) ? ((atoi(argv[6]) == 0) ? 0 : 1) : 0;
    } else {
        printf("ERROR: invalid cmd line!\n");
        return -1;
    }
    return 0;
}

int initCM(int& result)
{
    UINT version = 0;
#ifdef LINUX
    result = ::CreateCmDevice(pCmDev, version, va::display);
#else
    result = ::CreateCmDevice(pCmDev, version);
#endif
    if (result != CM_SUCCESS) {
        printf("ERROR: CmDevice creation error");
        return -1;
    }
    if (version < CM_1_0) {
        printf("ERROR: The runtime API version is later than runtime DLL version");
        return -1;
    }

    FILE* pFileISA = nullptr;
#ifdef LINUX
    pFileISA = fopen("downscale_genx.isa", "rb");
#else
    fopen_s(&pFileISA, "downscale_genx.isa", "rb");
#endif
    if (pFileISA == NULL) {
        printf("ERROR: failed to open downscale_genx.isa\n");
        return -1;
    }
    fseek(pFileISA, 0, SEEK_END);
    int codeSize = ftell(pFileISA);
    rewind(pFileISA);
    if (codeSize == 0) {
        printf("ERROR: invalid ISA file\n");
        return -1;
    }
    pCommonISACode = (BYTE*)malloc(codeSize);
    if (!pCommonISACode) {
        printf("ERROR: failed to allocate memory for ISA code, size = %d\n", codeSize);
        return -1;
    }
    if (fread(pCommonISACode, 1, codeSize, pFileISA) != codeSize) {
        printf("ERROR: failed to read ISA file\n");
        return -1;
    }
    fclose(pFileISA);

    result = pCmDev->LoadProgram(pCommonISACode, codeSize, pProgram);
    if (result != CM_SUCCESS) {
        printf("ERROR: CM LoadProgram error\n");
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[])
{
    if (cmdOpt(argc, argv)) {
        return -1;
    }

    int dstSizeY = dstW * dstH;
    int dstSize = dstW * dstH * 3 / 2;

    unsigned char* pDstMem;

#ifdef LINUX
    if (va::display == nullptr)
    {
        if (!va::openDisplay())
        {
            std::cout<<"cmds: initMDF failed due to m_va_dpy = NULL"<<std::endl;
            return -1;
        }
    }
#endif

    pDstMem = (unsigned char*)malloc(dstSize);
    if (NULL == pDstMem) {
        printf("ERROR: pDstMem alloc failed\n");
        return-1;
    }

#ifdef LINUX
    unsigned char* pSrcMem = (unsigned char*)memalign(0x1000, img.size);
#else
    unsigned char* pSrcMem = (unsigned char*)_aligned_malloc(img.size, 0x1000);
#endif
    if (pSrcMem)  {
        memset(pSrcMem, 0, img.size);
#ifdef LINUX
        memcpy(pSrcMem, img.buf, img.size);
#else
        memcpy_s(pSrcMem, img.size, img.buf, img.size);
#endif
    } else {
        printf("ERROR: alloc pSrcMem failed\n");
        return-1;
    }

    if (initCM(cmRet)) {
        return -1;
    }

    // Queue
    CmQueue* pCmQueue = NULL;
    cmRet = pCmDev->CreateQueue(pCmQueue);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateQueue error\n");
        return -1;
    }

    // Task
    CmTask* pTask = NULL;
    cmRet = pCmDev->CreateTask(pTask);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CmDevice CreateTask error\n");
        return -1;
    }

    // Kernel
    CmKernel* kernel = NULL;
    cmRet = pCmDev->CreateKernel(pProgram, gKernelNameList[kindex], kernel);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateKernel error\n");
        return -1;
    }
    int threadWidth = (dstW + DS_THREAD_WIDTH - 1) / DS_THREAD_WIDTH;
    int threadHeight = (dstH + DS_THREAD_HEIGHT - 1) / DS_THREAD_HEIGHT;
    kernel->SetThreadCount(threadWidth * threadHeight);
    printf("INFO: HW thread_w = %d, thread_h = %d, total = %d\n", threadWidth, threadHeight, threadWidth * threadHeight);

    // Thread Space
    CmThreadSpace* pTS = nullptr;
    cmRet = pCmDev->CreateThreadSpace(threadWidth, threadHeight, pTS);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateThreadSpace error\n");
        return -1;
    }

    // 3D Sampler
    CmSampler* pSampler = NULL;
    CM_SAMPLER_STATE  sampleState = {};
    SamplerIndex* samplerIndex = NULL;
    sampleState.magFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.minFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.addressU = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressV = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressW = CM_TEXTURE_ADDRESS_CLAMP;
    cmRet = pCmDev->CreateSampler(sampleState, pSampler);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSampler error\n");
        return -1;
    }
    pSampler->GetIndex(samplerIndex);

    // Src surface
    CmSurface2D* pSrcSurface = NULL;
    unsigned int pitch, physicalSize;
    SurfaceIndex* pSrcSurfaceIndex = NULL;
    if (((unsigned long long)pSrcMem & 0xfff) == 0) {
        pCmDev->GetSurface2DInfo(srcW, srcH, CM_SURFACE_FORMAT_NV12, pitch, physicalSize);
        printf("INFO: CmSurface2D w = %d, h = %d, pitch = %d, size = %d\n", srcW, srcH, pitch, physicalSize);
    }
    cmRet = pCmDev->CreateSurface2D(srcW, srcH, CM_SURFACE_FORMAT_NV12, pSrcSurface);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSurface2D error\n");
        return -1;
    }
    cmRet = pSrcSurface->WriteSurface(pSrcMem, NULL);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM WriteSurface error\n");
        return -1;
    }
    cmRet = pCmDev->CreateSamplerSurface2D(pSrcSurface, pSrcSurfaceIndex);
    if (cmRet != CM_SUCCESS) {
        printf("CM CreateSamplerUPSurface error");
        return -1;
    }

    // Dst surface
    CmSurface2D* pDstSurface = NULL;
    SurfaceIndex* pDstSurfIndex = NULL;
    cmRet = pCmDev->CreateSurface2D(dstW, dstH, CM_SURFACE_FORMAT_NV12, pDstSurface);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CM CreateSurface2D error\n");
        return -1;
    }
    pDstSurface->GetIndex(pDstSurfIndex);

    kernel->SetKernelArg(0, sizeof(SurfaceIndex), pSrcSurfaceIndex);
    kernel->SetKernelArg(1, sizeof(SamplerIndex), samplerIndex);
    kernel->SetKernelArg(2, sizeof(SurfaceIndex), pDstSurfIndex);
    kernel->SetKernelArg(3, sizeof(unsigned short), &dstW);
    kernel->SetKernelArg(4, sizeof(unsigned short), &dstH);

    cmRet = pTask->AddKernel(kernel);
    if (cmRet != CM_SUCCESS) {
        printf("ERROR: CmDevice AddKernel error\n");
        return -1;
    }

    double totalTime = 0.0;
    CmEvent* e = NULL;
    for (size_t i = 0; i < runNum; i++) {
        cmRet = pCmQueue->Enqueue(pTask, e, pTS);
        if (cmRet != CM_SUCCESS) {
            printf("ERROR: CmDevice enqueue error\n");
            return -1;
        }

        cmRet = pDstSurface->ReadSurface(pDstMem, e);
        if (cmRet != CM_SUCCESS) {
            printf("ERROR: CM ReadSurface error\n");
            return -1;
        }

        UINT64 executionTime = 0;
        cmRet = e->GetExecutionTime(executionTime);
        if (cmRet != CM_SUCCESS) {
            printf("CM GetExecutionTime error");
            return -1;
        }
        totalTime += executionTime;
        static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            dumpNV12("out.nv12", (char*)pDstMem, dstSize);
        }
    }

    printf("INFO: Average execution time: %f us; run_times = %d\n", totalTime/runNum/1000.0, runNum);

    pCmDev->DestroyTask(pTask);
    pCmDev->DestroySurface(pDstSurface);
    pCmDev->DestroySurface(pSrcSurface);
    pCmDev->DestroyKernel(kernel);
    pCmDev->DestroyThreadSpace(pTS);
    pCmDev->DestroyProgram(pProgram);
    pCmDev->DestroySampler(pSampler);
    ::DestroyCmDevice(pCmDev);
#ifdef LINUX
    free(pSrcMem);
#else
    _aligned_free(pSrcMem);
#endif
    free(pDstMem);
    free(pCommonISACode);

#ifdef LINUX
    if (va::display != nullptr)
        va::closeDisplay();
#endif

    printf("done!\n");
    return 0;
}
