/**
***
*** Copyright  (C) 1985-2011 Intel Corporation. All rights reserved.
***
*** The information and source code contained herein is the exclusive
*** property of Intel Corporation. and may not be disclosed, examined
*** or reproduced in whole or in part without explicit written authorization
*** from the company.
***
*** ----------------------------------------------------------------------------
**/

#include "cm_rt.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <stdio.h>
#include <fstream>

using namespace std;

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

#ifdef CMRT_EMU
extern "C" void downscale(SurfaceIndex src_idx, SamplerIndex sampler_idx, SurfaceIndex dst_idx, unsigned short image_width, unsigned short image_height);
#endif

struct ImgData {
    int w;
    int h;
    int size;
    char* buf;
    char* y;
    char* uv;
};

void readNV12(char*filename, ImgData &img)
{
    ifstream infile(filename, ios::binary);
    img.buf = new char[img.size];
    memset(img.buf, 0, img.size);
    infile.read(img.buf, img.size);
    img.y = img.buf;
    img.uv = img.buf + img.w * img.h;
    infile.close();
}

void dumpNV12(char* filename, char* buf, int size)
{
    ofstream of(filename, ios::binary);
    of.write(buf, size);
    of.close();
}

int main(int argc, char* argv[])
{
#ifdef CMRT_EMU
    printf("Applciation is running in emulation mode\n");
#else
    printf("Applciation is running in HW mode\n");
#endif

    int result;
    int width, height, DSWidth, DSHeight;
    bool bRealImage = false;
    ImgData img = {};
    if (argc == 1)
    {
        width = 1920;
        height = 1080;
        DSWidth = 300;
        DSHeight = 300;
        img.w = width;
        img.h = height;
        img.size = width * height * 3 / 2;
        bRealImage = true;
        readNV12("test5.nv12", img);
    }
    else if (argc == 6)
    {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
        DSWidth = atoi(argv[3]);
        DSHeight = atoi(argv[4]);
        img.w = width;
        img.h = height;
        img.size = width * height * 3 / 2;
        bRealImage = true;
        readNV12(argv[5], img);
    }
    else
    {
        printf("ERROR: invalid cmd line!\n");
        return -1;
    }
    int dstSizeY = DSWidth * DSHeight;
    int dstSize = DSWidth * DSHeight * 3 / 2;
    unsigned char* gpu_downscale;
    gpu_downscale = (unsigned char*)malloc(dstSize);
    if (NULL == gpu_downscale)
    {
        printf("gpu downscale alloc failed\n");
        return-1;
    }
    // allocate input buffer
    unsigned char* pSysMemSrc = (unsigned char*)_aligned_malloc(img.size, 0x1000);
    if (pSysMemSrc) 
    {
        memset(pSysMemSrc, 0, img.size);
        if (bRealImage)
        {
            memcpy_s(pSysMemSrc, img.size, img.buf, img.size);
            //dumpNV12("input.nv12", (char*)pSysMemSrc, width * height);
        }
    }
    else
    {
        printf("alloc pSysMemSrc fail\n");
        exit(1);
    }

    unsigned char* pSysMemRef = (unsigned char*)malloc(dstSize);
    if (pSysMemRef)
    {
        memset(pSysMemRef, 0, dstSize);
    }
    else
    {
        printf("alloc pSysMemRef fail\n");
        exit(1);
    }

    // Create a CM Device
    CmDevice* pCmDev = NULL;;
    UINT version = 0;

    result = ::CreateCmDevice(pCmDev, version);
    if (result != CM_SUCCESS) {
        printf("CmDevice creation error");
        return -1;
    }
    if (version < CM_1_0) {
        printf(" The runtime API version is later than runtime DLL version");
        return -1;
    }

    FILE* pISA = nullptr;
    fopen_s(&pISA, "downscale_genx.isa", "rb");
    if (pISA == NULL) {
        perror("downscale_genx.isa");
        return -1;
    }

    fseek(pISA, 0, SEEK_END);
    int codeSize = ftell(pISA);
    rewind(pISA);

    if (codeSize == 0)
    {
        return -1;
    }

    void* pCommonISACode = (BYTE*)malloc(codeSize);
    if (!pCommonISACode)
    {
        return -1;
    }

    if (fread(pCommonISACode, 1, codeSize, pISA) != codeSize) {
        perror("downscale_genx.isa");
        return -1;
    }
    fclose(pISA);


    CmProgram* program = NULL;
    result = pCmDev->LoadProgram(pCommonISACode, codeSize, program);
    if (result != CM_SUCCESS) {
        perror("CM LoadProgram error");
        return -1;
    }

    // Create a task queue
    CmQueue* pCmQueue = NULL;
    result = pCmDev->CreateQueue(pCmQueue);
    if (result != CM_SUCCESS) {
        perror("CM CreateQueue error");
        return -1;
    }

    // Create a kernel
    CmKernel* kernel = NULL;
    result = pCmDev->CreateKernel(program, CM_KERNEL_FUNCTION(downscale), kernel);
    if (result != CM_SUCCESS) {
        perror("CM CreateKernel error");
        return -1;
    }

    CmSampler* pSampler;

    //create sampler for downsacale
    CM_SAMPLER_STATE  sampleState;
    sampleState.magFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.minFilterType = CM_TEXTURE_FILTER_TYPE_LINEAR;
    sampleState.addressU = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressV = CM_TEXTURE_ADDRESS_CLAMP;
    sampleState.addressW = CM_TEXTURE_ADDRESS_CLAMP;
    result = pCmDev->CreateSampler(sampleState, pSampler);
    if (result != CM_SUCCESS)
    {
        printf("CM CreateSampler error");
        return -1;
    }

    CmSurface2D* pBaseImageSurface = NULL;
    CmSurface2DUP* pBaseImageSurfaceUP = NULL;

    unsigned int pitch, physicalSize;
    bool useUDBuffer = false;
    SurfaceIndex* pBaseImageIndex = NULL;
    if (((unsigned int)pSysMemSrc & 0xfff) == 0)
    {
        pCmDev->GetSurface2DInfo(width, height, CM_SURFACE_FORMAT_NV12, pitch, physicalSize);
        if (physicalSize == width * height)
        {
            useUDBuffer = true;
        }
    }

    // create buffer for the base image
    result = pCmDev->CreateSurface2D(width, height, CM_SURFACE_FORMAT_NV12, pBaseImageSurface);
    if (result != CM_SUCCESS) {
        printf("CM CreateSurface error");
        return -1;
    }

    // use CPU copy
    result = pBaseImageSurface->WriteSurface(pSysMemSrc, NULL);
    if (result != CM_SUCCESS) {
        printf("CM CreateKernel error");
        return -1;
    }

    result = pCmDev->CreateSamplerSurface2D(pBaseImageSurface, pBaseImageIndex);
    if (result != CM_SUCCESS) {
        printf("CM CreateSamplerUPSurface error");
        return -1;
    }

    CmSurface2D* pDownscaleSurface = NULL;
    SurfaceIndex* pDownscaleIndex = NULL;

    result = pCmDev->CreateSurface2D(DSWidth, DSHeight, CM_SURFACE_FORMAT_NV12, pDownscaleSurface);
    if (result != CM_SUCCESS)
    {
        printf("CM CreateSurface2D error");
        return -1;
    }
    pDownscaleSurface->GetIndex(pDownscaleIndex);

    int threadswidth = (DSWidth + DS_THREAD_WIDTH - 1) / DS_THREAD_WIDTH;
    int threadsheight = (DSHeight + DS_THREAD_HEIGHT - 1) / DS_THREAD_HEIGHT;

    printf("HW thread_w = %d, thread_h = %d, total = %d\n", threadswidth, threadsheight, threadswidth * threadsheight);

    kernel->SetThreadCount(threadswidth * threadsheight);

    CmThreadSpace* pTS;
    result = pCmDev->CreateThreadSpace(threadswidth, threadsheight, pTS);
    if (result != CM_SUCCESS) {
        printf("CM CreateThreadSpace error");
        return -1;
    }

    // arg 0 - base image
    kernel->SetKernelArg(0, sizeof(SurfaceIndex), pBaseImageIndex);

    // arg 1 - sampler index
    SamplerIndex* samplerIndex = NULL;
    pSampler->GetIndex(samplerIndex);
    kernel->SetKernelArg(1, sizeof(SamplerIndex), samplerIndex);

    // arg 2 - images
    kernel->SetKernelArg(2, sizeof(SurfaceIndex), pDownscaleIndex);

    // arg 3 - images width
    kernel->SetKernelArg(3, sizeof(unsigned short), &DSWidth);

    // arg 4 - images height
    kernel->SetKernelArg(4, sizeof(unsigned short), &DSHeight);

    CmTask* pKernelArray = NULL;

    result = pCmDev->CreateTask(pKernelArray);
    if (result != CM_SUCCESS) {
        printf("CmDevice CreateTask error");
        return -1;
    }

    result = pKernelArray->AddKernel(kernel);
    if (result != CM_SUCCESS) {
        printf("CmDevice AddKernel error");
        return -1;
    }

    CmEvent* e = NULL;

    int num = 1000;
    double totalTime = 0.0;
    for (size_t i = 0; i < num; i++)
    {
        result = pCmQueue->Enqueue(pKernelArray, e, pTS);
        if (result != CM_SUCCESS) {
            printf("CmDevice enqueue error");
            return -1;
        }

        result = pDownscaleSurface->ReadSurface(gpu_downscale, e);
        if (result != CM_SUCCESS) {
            printf("CM ReadSurface error");
            return -1;
        }

        UINT64 executionTime = 0;
        result = e->GetExecutionTime(executionTime);
        if (result != CM_SUCCESS) {
            printf("CM GetExecutionTime error");
            return -1;
        }
        else
        {
            //printf("Kernel linear execution time is %ld nanoseconds\n", executionTime);
        }

        totalTime += executionTime;

        if (bRealImage)
        {
            static int dumped = 0;
            if (!dumped) 
            {
                dumped = 1;
                dumpNV12("out.nv12", (char*)gpu_downscale, dstSize);
            }
        }
    }

    printf("Average execution time: %f us; thread_num = %d\n", totalTime/num/1000.0, threadswidth* threadsheight);

    pCmDev->DestroyTask(pKernelArray);

    // destroy surfaces
    pCmDev->DestroySurface(pDownscaleSurface);

    if (pBaseImageSurface)
    {
        pCmDev->DestroySurface(pBaseImageSurface);
    }

    if (pBaseImageSurfaceUP)
    {
        pCmDev->DestroySurface2DUP(pBaseImageSurfaceUP);
    }

    //destroy downscale kernels
    pCmDev->DestroyKernel(kernel);
    pCmDev->DestroyThreadSpace(pTS);

    free(pCommonISACode);
    pCmDev->DestroyProgram(program);

    // destory sampler
    pCmDev->DestroySampler(pSampler);

    // destroy CM device
    ::DestroyCmDevice(pCmDev);

    _aligned_free(pSysMemSrc);
    free(gpu_downscale);
    free(pSysMemRef);

    printf("success!\n");
    return 0;
}
