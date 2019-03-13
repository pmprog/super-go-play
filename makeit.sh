#!/bin/bash

echo "Compile gnuboy"
cd gnuboy-go
make -j4 all
cp ./build/gnuboy-go.bin ..
cd ..

echo "Compile nesemu"
cd nesemu-go
make -j4 all
cp ./build/nesemu-go.bin ..
cd ..

echo "Compile smsplusgx-go"
cd smsplusgx-go
make -j4 all
cp ./build/smsplusgx-go.bin ..
cd ..

if [ ! -f tile.raw ]; then
	echo "Making image"
	ffmpeg -i tile.png -f rawvideo -pix_fmt rgb565 tile.raw
fi

echo "Compiling .fw"
../odroid-go-firmware/tools/mkfw/mkfw "Super Go Play" tile.raw 0 16 1048576 springboard ripped/springboard.bin 0 17 1048576 nesemu nesemu-go.bin 0 18 1048576 gnuboy gnuboy-go.bin 0 19 2097152 smsplusgx smsplusgx-go.bin
mv firmware.fw supergoplay.fw

if [ ! -f nesemu-go.bin ]; then
	echo "NES EMU failed to build"
fi

if [ ! -f gnuboy-go.bin ]; then
	echo "GB EMU failed to build"
fi

if [ ! -f smsplusgx-go.bin ]; then
	echo "SMS EMU failed to build"
fi


rm gnuboy-go.bin
rm nesemu-go.bin
rm smsplusgx-go.bin
