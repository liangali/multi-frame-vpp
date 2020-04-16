// 116_CmTask_MultiContext.cpp : Defines the entry point for the console application.
//

#include "cm_rt.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <stdio.h>
#include <fstream>

#define VECTOR_INITIALIZATION_WORKAROUND 1

//below macro will be used to set the number of compute context. maximum number is 4.
#define CCS_NUM 4

using namespace std;

#define TEST_SLMSIZE_PAGES 1

static const int LOOP_COUNT = 3;

int testGPUWalker(int threadWidth, int threadHeight, int groupWidth,
                  int groupHeight, int SLMSizeInPage)
{
    int result = 0;

    UINT threadNum = threadWidth * threadHeight * groupWidth * groupHeight;

    UINT bufferWidth = threadNum * 64;

    unsigned char *rcs_input_data = new unsigned char[bufferWidth];
    unsigned char *rcs_output_data = new unsigned char[bufferWidth];
    unsigned char *ccs_input_data[CCS_NUM];
    unsigned char *ccs_output_data[CCS_NUM];
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ccs_idx++)
    {
        ccs_input_data[ccs_idx] = new unsigned char[bufferWidth];
        ccs_output_data[ccs_idx] = new unsigned char[bufferWidth];
    }

    //Initialize input buffer, may read from file, here just a random value
    for (unsigned int i = 0; i < bufferWidth; ++i)
    {
        int temp = rand();
        rcs_input_data[i] = temp & 0x000000FF;
        rcs_output_data[i] = (temp & 0x0000FF00) >> 8;

        for (int ccs_idx = 0; ccs_idx < CCS_NUM; ccs_idx++)
        {
            ccs_input_data[ccs_idx][i] = temp & 0x000000FF;
            ccs_output_data[ccs_idx][i] = (temp & 0x0000FF00) >> 8;
        }
    }

    // Create a CM Device
    CmDevice* pCmDev = NULL;;
    UINT version = 0;
    result = ::CreateCmDevice( pCmDev, version );
    if (result != CM_SUCCESS )
    {
        perror("CmDevice creation error");
        return -1;
    }
    if (version < CM_2_0 )
    {
        perror(" The runtime API version is later than runtime DLL version");
        return -1;
    }

    CmBuffer *rcs_input_buffer = nullptr;
    CmBuffer *rcs_output_buffer = nullptr;
    result = pCmDev->CreateBuffer(bufferWidth, rcs_input_buffer);
    if (result != CM_SUCCESS)
    {
        perror("CM CreateSurface2D error");
        return -1;
    }
    result = rcs_input_buffer->WriteSurface(rcs_input_data, NULL);
    if (result != CM_SUCCESS)
    {
        perror("CM WriteSurface error");
        return -1;
    }
    result = pCmDev->CreateBuffer(bufferWidth, rcs_output_buffer);
    if (result != CM_SUCCESS)
    {
        perror("CM CreateSurface2D error");
        return -1;
    }

    CmBuffer *ccs_input_buffers[CCS_NUM] = {nullptr};
    CmBuffer *ccs_output_buffers[CCS_NUM] = {nullptr};
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        result = pCmDev->CreateBuffer(bufferWidth, ccs_input_buffers[ccs_idx]);
        if (result != CM_SUCCESS)
        {
            perror("CM CreateSurface2D error");
            return -1;
        }
        result = ccs_input_buffers[ccs_idx]->WriteSurface(ccs_input_data[ccs_idx],
                                                          nullptr);
        if (result != CM_SUCCESS)
        {
            perror("CM WriteSurface error");
            return -1;
        }

        result = pCmDev->CreateBuffer(bufferWidth, ccs_output_buffers[ccs_idx]);
        if (result != CM_SUCCESS)
        {
            perror("CM CreateBuffer error");
            return -1;
        }
    }

    ifstream is;
    is.open( "116_CmTask_MultiContext_genx.isa", ios::binary );
    if (!is.good() )
    {
        perror("Open 116_CmTask_MultiContext_genx.isa failed.");
        return -1;
    }
    is.seekg (0, ios::end);
    uint32_t codeSize = static_cast<uint32_t>(is.tellg());
    is.seekg (0, ios::beg);
    if(codeSize == 0)
    {
        return -1;
    }
    void *pCommonISACode = new BYTE[ codeSize ];
    if( !pCommonISACode )
    {
        return -1;
    }
    is.read((char*)pCommonISACode, codeSize);
    is.close();

    CmProgram *program = NULL;
    result = pCmDev->LoadProgram(pCommonISACode, codeSize, program);
    if (result != CM_SUCCESS )
    {
        perror("CM LoadProgram error");
        return -1;
    }

    // Create a kernel
    CmKernel *kernelRcs = nullptr;
    result = pCmDev->CreateKernel(program, CM_KERNEL_FUNCTION(cmGPUWalkerTest),
                                  kernelRcs);
    if (result != CM_SUCCESS)
    {
        perror("CM CreateKernel error");
        return -1;
    }
    SurfaceIndex *indexRcs0 = nullptr;
    rcs_input_buffer->GetIndex(indexRcs0);
    kernelRcs->SetKernelArg(0, sizeof(SurfaceIndex), indexRcs0);
    SurfaceIndex *indexRcs1 = nullptr;
    rcs_output_buffer->GetIndex(indexRcs1);
    kernelRcs->SetKernelArg(1, sizeof(SurfaceIndex), indexRcs1);

    CmKernel *kernelCcs[CCS_NUM] = { nullptr };
    SurfaceIndex *indexCcs_0[CCS_NUM] = { nullptr };
    SurfaceIndex *indexCcs_1[CCS_NUM] = { nullptr };
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        result = pCmDev->CreateKernel(program, CM_KERNEL_FUNCTION(cmGPUWalkerTest),
                                      kernelCcs[ccs_idx]);
        if (result != CM_SUCCESS)
        {
            perror("CM CreateKernel error");
            return -1;
        }

        ccs_input_buffers[ccs_idx]->GetIndex(indexCcs_0[ccs_idx]);
        kernelCcs[ccs_idx]->SetKernelArg(0, sizeof(SurfaceIndex), indexCcs_0[ccs_idx]);

        ccs_output_buffers[ccs_idx]->GetIndex(indexCcs_1[ccs_idx]);
        kernelCcs[ccs_idx]->SetKernelArg(1, sizeof(SurfaceIndex), indexCcs_1[ccs_idx]);
    }

    CmThreadGroupSpace *rcs_thread_group = nullptr;
    pCmDev->CreateThreadGroupSpace(threadWidth, threadHeight, groupWidth,
                                   groupHeight, rcs_thread_group);
    CmThreadGroupSpace *ccs_thread_groups[CCS_NUM] = { nullptr };
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ccs_idx++)
    {
        pCmDev->CreateThreadGroupSpace(threadWidth, threadHeight, groupWidth,
                                       groupHeight, ccs_thread_groups[ccs_idx]);
    }

    // Create a task queue
    CmQueue *queue_render = nullptr;
    CM_QUEUE_CREATE_OPTION queue_option = {};
    queue_option.QueueType = CM_QUEUE_TYPE_RENDER;
    result = pCmDev->CreateQueueEx(queue_render, queue_option);
    if (result != CM_SUCCESS)
    {
        perror("CM CreateQueue error");
        return -1;
    }

    CmQueue *queue_compute[CCS_NUM] = { nullptr };
    queue_option.QueueType = CM_QUEUE_TYPE_COMPUTE;
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        result = pCmDev->CreateQueueEx(queue_compute[ccs_idx], queue_option);
        if (result != CM_SUCCESS)
        {
            perror("CM CreateQueue error");
            return -1;
        }
    }

    CmTask *rcs_task = nullptr;
    result = pCmDev->CreateTask(rcs_task);
    if (result != CM_SUCCESS)
    {
        perror("CmDevice CreateTask error");
        return -1;
    }
    result = rcs_task->AddKernel(kernelRcs);
    if (result != CM_SUCCESS)
    {
        perror("CmDevice AddKernel error");
        return -1;
    }

    CmTask *ccs_tasks[CCS_NUM] = { NULL };
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        result = pCmDev->CreateTask(ccs_tasks[ccs_idx]);
        if (result != CM_SUCCESS)
        {
            perror("CmDevice CreateTask error");
            return -1;
        }
        result = ccs_tasks[ccs_idx]->AddKernel(kernelCcs[ccs_idx]);
        if (result != CM_SUCCESS)
        {
            perror("CmDevice AddKernel error");
            return -1;
        }
    }

    CmEvent *ccs_events[LOOP_COUNT][CCS_NUM];
    printf("Loop begins.\n");
    for (int loop = 0; loop < LOOP_COUNT; ++loop)
    {
        for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
        {
            result = queue_compute[ccs_idx]
                    ->EnqueueWithGroup(ccs_tasks[ccs_idx],
                                       ccs_events[loop][ccs_idx],
                                       ccs_thread_groups[ccs_idx]);
            if (result != CM_SUCCESS)
            {
                printf("CmDevice CCS %d enqueue error", ccs_idx);
                return -1;
            }
        }

#ifdef USE_RCS
        result = queue_render->EnqueueWithGroup(rcs_task, rcs_event[loop],
                                                rcs_thread_group);
        if (result != CM_SUCCESS)
        {
            perror("RCS enqueue error");
            return -1;
        }
#endif  // #ifdef USE_RCS
    }

    for (int loop = 0; loop < LOOP_COUNT; ++loop)
    {
        printf("Now waiting for tasks in Loop %d to finish.\n", loop);
        for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
        {
            result = ccs_events[loop][ccs_idx]->WaitForTaskFinished(0xFFFFFFFF);
            if (CM_SUCCESS != result)
            {
                printf("WaitForTaskFinished() failed with error %d.\n", result);
            }
            printf("Compute queue is 0x%llx, event is 0x%llx.\n",
                   reinterpret_cast<uint64_t>(queue_compute[ccs_idx]),
                   reinterpret_cast<uint64_t>(ccs_events[loop][ccs_idx]));
            queue_compute[ccs_idx]->DestroyEvent(ccs_events[loop][ccs_idx]);
        }

#ifdef USE_RCS
        int32_t result = rcs_event[loop]->WaitForTaskFinished(0xFFFFFFFF);
        if (CM_SUCCESS != result)
        {
            printf("WaitForTaskFinished() failed with error %d.\n", result);
        }
        printf("Render queue is 0x%llx, event is 0x%llx.\n",
               reinterpret_cast<uint64_t>(queue_render),
               reinterpret_cast<uint64_t>(rcs_event[loop]));
        queue_render->DestroyEvent(rcs_event[loop]);
#endif  // #ifdef USE_RCS
    }

    printf("Now destroying tasks.\n");
    pCmDev->DestroyTask(rcs_task);
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        pCmDev->DestroyTask(ccs_tasks[ccs_idx]);
    }

    printf("Now reading surfaces.\n");
#ifdef USE_RCS
    result = rcs_output_buffer->ReadSurface(rcs_output_data, nullptr);
    if (result != CM_SUCCESS)
    {
        perror("CM ReadSurface error");
        return -1;
    }
#endif  // #ifdef USE_RCS
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        result = ccs_output_buffers[ccs_idx]
                ->ReadSurface(ccs_output_data[ccs_idx], nullptr);
        if (result != CM_SUCCESS)
        {
            perror("CM ReadSurface error");
            return -1;
        }
    }

    pCmDev->DestroyThreadGroupSpace(rcs_thread_group);
    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ccs_idx++)
    {
        pCmDev->DestroyThreadGroupSpace(ccs_thread_groups[ccs_idx]);
    }

    printf("Now destroying CM device.\n");
    result = ::DestroyCmDevice( pCmDev );

    delete[] pCommonISACode;
    result = 0;

#ifdef USE_RCS
    if (memcmp(rcs_output_data, rcs_input_data, bufferWidth))
    {
        result = result | -1;
    }
#endif  // #ifdef USE_RCS
    delete[] rcs_output_data;
    delete[] rcs_input_data;

    for (int ccs_idx = 0; ccs_idx < CCS_NUM; ++ccs_idx)
    {
        if (memcmp(ccs_input_data[ccs_idx], ccs_output_data[ccs_idx],
                   bufferWidth))
        {
            result = result | -1;
        }
        delete[] ccs_input_data[ccs_idx];
        delete[] ccs_output_data[ccs_idx];
    }

    return result;
}

int main (int argc, char * argv[])
{
#ifdef CMRT_EMU
    std::cout << "Applciation is running in emulation mode" << std::endl;
#else
    std::cout << "Applciation is running in HW mode" << std::endl;
#endif

    int result = 0;

    result = testGPUWalker(1, 1, 128, 128, 0);

    if(result == 0)
    {
        printf("PASSED\n");
    }
    else
    {
        printf("FAILED\n");
    }
    return result;
}
