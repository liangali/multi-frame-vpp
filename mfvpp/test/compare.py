import numpy as np
import os

w, h = 300, 300

def tobmp(w, h, fmt, infile):
    outfile = infile.split('.')[0] + '.bmp' 
    cmd = 'ffmpeg -y -s ' + str(w) + 'x' + str(h) + ' -pix_fmt ' + fmt + ' -f rawvideo -i ' + infile + ' ' + outfile
    os.system(cmd)

a = np.fromfile('out1.nv12', dtype=np.int8)
b = np.fromfile('out2.nv12', dtype=np.int8)
c = np.absolute(b - a)

with open('diff.nv12', 'wb') as f:
    c.tofile(f)
tobmp(w, h, 'nv12', 'diff.nv12')

dy = c[0:w*h]
with open('diff_y.bin', 'wb') as f:
    dy.tofile(f)
tobmp(w, h, 'gray', 'diff_y.bin')

duv = c[w*h:].reshape((h//2, w//2, 2))
du = np.zeros((h//2, w//2), dtype=np.int8)
dv = np.zeros((h//2, w//2), dtype=np.int8)
for j in range(h//2):
    for i in range(w//2):
        du[j][i] = duv[j][i][0]
        dv[j][i] = duv[j][i][1]

with open('diff_u.bin', 'wb') as f:
    du.tofile(f)
tobmp(w//2, h//2, 'gray', 'diff_u.bin')

with open('diff_v.bin', 'wb') as f:
    dv.tofile(f)
tobmp(w//2, h//2, 'gray', 'diff_v.bin')

print('done')