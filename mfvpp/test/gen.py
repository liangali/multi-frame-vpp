import numpy as np

w, h = 64, 64

y= np.full((w, h), 127, dtype=np.int8)
u= np.full((w//2, h//2), 1, dtype=np.int8)
v= np.full((w//2, h//2), 8, dtype=np.int8)
uv = np.zeros((w//2, h//2, 2), dtype=np.int8)

for j in range(h//2):
    for i in range(w//2):
        uv[j][i][0] = u[j][i]
        uv[j][i][1] = v[j][i]

with open ("test.nv12", 'wb') as f:
    y.tofile(f)
    uv.tofile(f)

print('done')