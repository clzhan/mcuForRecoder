gcc -o player ffplay.c cmdutils.c  -I./../source_ffmpeg/ffmpeg-0.7.12/ -L./ -lavformat -lavcodec -lavfilter -lavutil -lpostproc -lswscale -lavdevice -I./sdl/include/SDL/ -L./sdl/lib/ -lSDL

sudo vi /etc/ld.so.conf
sudo ldconfig -v
