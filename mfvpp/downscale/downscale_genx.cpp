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

#define DS_THREAD_WIDTH 16
#define DS_THREAD_HEIGHT 16

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////// Downscale ///////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////



extern "C" _GENX_MAIN_ void
downscale(SurfaceIndex src_idx,
          SamplerIndex sampler_idx,
          SurfaceIndex dst_idx,
          ushort image_width, 
          ushort image_height)
{ 
    ushort x = get_thread_origin_x() * DS_THREAD_WIDTH;
    ushort y = get_thread_origin_y() * DS_THREAD_HEIGHT; 

    // calc interval
    float x_interval = 1.0f / (image_width-1);
    float y_interval = 1.0f / (image_height-1);

    float x0 = x_interval * x;
    float y0 = y_interval * y;
    
    matrix<uchar, DS_THREAD_HEIGHT, DS_THREAD_WIDTH> out;
    vector<ushort, 32> sampler_out;

#pragma unroll
    for (int i=0; i<DS_THREAD_HEIGHT/4; ++i) 
    {
#pragma unroll
        for (int j=0; j<DS_THREAD_WIDTH/8; ++j) 
        {
            sample32(sampler_out, 
                 CM_A_ENABLE, 
                 src_idx,
                 sampler_idx,
                 x0 + (8*j*x_interval),
                 y0 + (4*i*y_interval),
                 x_interval,
                 y_interval);

            sampler_out += 128;
            out.select<4,1,8,1>(i*4,j*8) = sampler_out.format<uchar>().select<32, 2>(1);
        }
    }

    write(dst_idx, x, y, out);
}
