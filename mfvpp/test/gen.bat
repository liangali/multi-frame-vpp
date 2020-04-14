
ffmpeg -f lavfi -i testsrc2 -s 1920x1080  -pix_fmt nv12 -vframes 1 -f rawvideo test.nv12 -y
ffmpeg -f lavfi -i testsrc2 -s 600x600    -pix_fmt nv12 -vframes 1 -f rawvideo test2.nv12 -y
ffmpeg -f lavfi -i testsrc2 -s 512x512    -pix_fmt nv12 -vframes 1 -f rawvideo test3.nv12 -y
ffmpeg -f lavfi -i testsrc2 -s 480x480    -pix_fmt nv12 -vframes 1 -f rawvideo test4.nv12 -y
ffmpeg -f lavfi -i testsrc2 -s 128x128    -pix_fmt nv12 -vframes 1 -f rawvideo test5.nv12 -y
ffmpeg -f lavfi -i testsrc2 -s 64x64      -pix_fmt nv12 -vframes 1 -f rawvideo test6.nv12 -y
