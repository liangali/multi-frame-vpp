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

#include <cm/cm.h>

#define	VECTOR_INITIALIZATION_WORKAROUND 1

extern "C" _GENX_MAIN_ void
cmGPUWalkerTest (SurfaceIndex IN, SurfaceIndex OUT)
{
    vector<uint, 16> current_vector(0);
    vector<uint, 16> prev_vector(0);
    vector<uint, 16> next_vector(0);
    int gThreadID = cm_linear_global_id();

    read(IN, gThreadID*64, current_vector);
    for (uint i = 0; i < 20; ++i)
    {
        if (i % 2 == 0)
        {
            read(IN, (gThreadID - 1)*64, prev_vector);
            read(IN, (gThreadID + 1)*64, next_vector);
            current_vector += prev_vector[15] + next_vector[0];
        }
        else
        {
            read(IN, (gThreadID - 1)*64, prev_vector);
            read(IN, (gThreadID + 1)*64, next_vector);
            current_vector -= prev_vector[15] + next_vector[0];
        }
    }

    write(OUT, gThreadID * 64, current_vector);
    return;
}
