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

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

#ifdef CMRT_EMU
extern "C" void downscale(SurfaceIndex src_idx, SamplerIndex sampler_idx, SurfaceIndex dst_idx, unsigned short image_width, unsigned short image_height);
#endif

int main(int argc, char* argv[])
{
#ifdef CMRT_EMU
    printf("Applciation is running in emulation mode\n");
#else
    printf("Applciation is running in HW mode\n");
#endif

    unsigned int factor = 1;
    if (argc == 2)
    {
        factor = atoi(argv[1]);
    }

    int result;
    unsigned int base_w = 16, base_h = 16;
    unsigned int width = base_w * 2 * factor;
    unsigned int height = base_h * 2 * factor;
    unsigned int DSWidth = base_w * factor;
    unsigned int DSHeight = base_h * factor;

    unsigned char* gpu_downscale;
    gpu_downscale = (unsigned char*)malloc(DSWidth * DSHeight);
    if (NULL == gpu_downscale)
    {
        printf("gpu downscale alloc failed\n");
        return-1;
    }
    // allocate input buffer
    unsigned char* pSysMemSrc = (unsigned char*)_aligned_malloc(width * height, 0x1000);
    if (pSysMemSrc) 
    {
        memset(pSysMemSrc, 123, width * height);
    }
    else
    {
        printf("alloc pSysMemSrc fail\n");
        exit(1);
    }

    unsigned char* pSysMemRef = (unsigned char*)malloc(DSWidth * DSHeight);
    if (pSysMemRef)
    {
        memset(pSysMemRef, 123, DSWidth * DSHeight);
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

    CmSurface2D* pBaseImageSurface;
    CmSurface2DUP* pBaseImageSurfaceUP;

    unsigned int pitch, physicalSize;
    bool useUDBuffer = false;
    SurfaceIndex* pBaseImageIndex;
    if (((unsigned int)pSysMemSrc & 0xfff) == 0)
    {
        pCmDev->GetSurface2DInfo(width, height, CM_SURFACE_FORMAT_A8, pitch, physicalSize);
        if (physicalSize == width * height)
        {
            useUDBuffer = true;
        }
    }

    if (useUDBuffer)
    {
        // create buffer for the base image
        result = pCmDev->CreateSurface2DUP(width, height, CM_SURFACE_FORMAT_A8, pSysMemSrc, pBaseImageSurfaceUP);
        if (result != CM_SUCCESS) {
            printf("CM CreateSurface error");
            return -1;
        }

        //pBaseImageSurfaceUP->GetIndex(pBaseImageIndex);
        pBaseImageSurface = NULL;

        SurfaceIndex* indexInputNV12 = NULL;

        result = pCmDev->CreateSamplerSurface2DUP(pBaseImageSurfaceUP, pBaseImageIndex);
        if (result != CM_SUCCESS) {
            printf("CM CreateSamplerUPSurface error");
            return -1;
        }
        printf("Call 2DUP sampler surface creation\n");
    }
    else
    {

        // create buffer for the base image
        result = pCmDev->CreateSurface2D(width, height, CM_SURFACE_FORMAT_A8, pBaseImageSurface);
        if (result != CM_SUCCESS) {
            printf("CM CreateSurface error");
            return -1;
        }

        // copy the image to the base surface
        // if both the width and pointer are 64 bytes aligned use the fast CM copy
        if ((((int)pSysMemSrc & 63) == 0) && (((int)pSysMemSrc & 63) == 0))
            //if(0)
        {
            CmEvent* e;
            result = pCmQueue->EnqueueCopyCPUToGPU(pBaseImageSurface, pSysMemSrc, e);
            if (result != CM_SUCCESS) {
                printf("CM CopyCPUToGPU error");
                return -1;
            }
            pCmQueue->DestroyEvent(e);
        }
        else
        {
            // use CPU copy
            result = pBaseImageSurface->WriteSurface(pSysMemSrc, NULL);
            if (result != CM_SUCCESS) {
                printf("CM CreateKernel error");
                return -1;
            }
        }

        pBaseImageSurface->GetIndex(pBaseImageIndex);
        pBaseImageSurfaceUP = NULL;
    }

    CmSurface2D* pDownscaleSurface;
    SurfaceIndex* pDownscaleIndex;

    result = pCmDev->CreateSurface2D(DSWidth, DSHeight, CM_SURFACE_FORMAT_A8, pDownscaleSurface);
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

        if (0 != memcmp(gpu_downscale, pSysMemRef, DSWidth * DSHeight))
        {
            perror("mismatch!");
            exit(1);
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
