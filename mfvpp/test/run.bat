
set w=1920
set h=1080
set w2=300
set h2=300

del test.nv12
ffmpeg -f lavfi -i testsrc2 -s %w%x%h% -pix_fmt nv12 -vframes 1 -f rawvideo test.nv12 -y
ffmpeg -s %w%x%h% -pix_fmt nv12 -f rawvideo -i test.nv12 test.bmp -y

del out.nv12
downscale.exe %w% %h% %w2% %h2% test.nv12

del out.bmp
ffmpeg -s %w2%x%h2% -pix_fmt nv12 -f rawvideo -i out.nv12 out.bmp -y

rem code out.bmp
