cmake_minimum_required(VERSION 3.14)
project(ffplay)

set(CMAKE_CXX_STANDARD 11)

set(FFMPEG_DIR /usr/local/Cellar/ffmpeg/4.2.2_2)
include_directories(${FFMPEG_DIR}/include/)
link_directories(${FFMPEG_DIR}/lib/)

set(SDL_DIR /usr/local/Cellar/sdl2/2.0.12)
include_directories(${SDL_DIR}/include/SDL2/)
link_directories(${SDL_DIR}/lib/)

add_executable(ffplay main.c cmdutils.c packet_queue.c frame_queue.c decoder.c sclock.c opts.c media_state.c)

target_link_libraries(
        ffplay
        avcodec
        avdevice
        avfilter
        avformat
        swscale
        avutil
        swresample
        postproc
        sdl2
)