#include <libcamera/camera_manager.h>
#include <libcamera/camera.h>
#include <libcamera/stream.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/framebuffer.h>
#include <libcamera/request.h>
#include <libcamera/formats.h>
#include <libcamera/control_ids.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xpresent.h>
#include <sys/mman.h>

extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavformat/avformat.h>
  #include <libavutil/opt.h>
  #include <libavutil/imgutils.h>
}

#include <iostream>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>
#include <jpeglib.h>
#include <mutex>
#include <cstdio>
#include <ctime>
#include <chrono>

Display *display;
Window window;
GC gc;
XImage *image;
Pixmap pixmap[2];
int currentPixmap = 0;


void yuv420_to_rgb(
    const uint8_t *y,
    const uint8_t *u,
    const uint8_t *v,
    uint8_t *rgb,
    int width,
    int height
);

using namespace libcamera;

struct MappedPlane {
    void *mapAddress;
    uint8_t *data;
    size_t mapLength;
};

struct MappedBuffer {
    FrameBuffer *buffer;
    std::vector<MappedPlane> planes;
};

struct YuvFrame {
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    YuvFrame()
        : y(640 * 480),
          u(320 * 240),
          v(320 * 240) {}
};

class VideoRecorder {
public:
    AVFormatContext *formatContext = nullptr;
    AVCodecContext *codecContext = nullptr;
    AVStream *videoStream = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    std::chrono::steady_clock::time_point recordingStart;
    std::chrono::steady_clock::time_point blinkTimer;
    bool dotVisible = true;
    bool recording = false;
    int64_t frameNumber = 0;

    bool start() {
        time_t now = std::time(nullptr);
        tm *localTime = std::localtime(&now);
        blinkTimer = std::chrono::steady_clock::now();

        char filename[128];

        std::strftime(
            filename,
            sizeof(filename),
            "/home/antam/Videos/video_%Y-%m-%d_%H-%M-%S.mp4",
            localTime
        );

        const AVCodec *codec = avcodec_find_encoder_by_name("libx264");

        if (!codec)
            return false;

        if (avformat_alloc_output_context2(
                &formatContext,
                nullptr,
                "mp4",
                filename) < 0)
            return false;

        videoStream = avformat_new_stream(formatContext, nullptr);

        if (!videoStream)
            return false;

        codecContext = avcodec_alloc_context3(codec);

        if (!codecContext)
            return false;

        codecContext->codec_id = AV_CODEC_ID_H264;
        codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
        codecContext->width = 640;
        codecContext->height = 480;
        codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
        codecContext->time_base = AVRational{1, 30};
        codecContext->framerate = AVRational{30, 1};
        codecContext->bit_rate = 4000000;
        codecContext->gop_size = 30;
        codecContext->max_b_frames = 0;

        av_opt_set(codecContext->priv_data, "preset", "ultrafast", 0);
        av_opt_set(codecContext->priv_data, "tune", "zerolatency", 0);

        if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
            codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (avcodec_open2(codecContext, codec, nullptr) < 0)
            return false;

        if (avcodec_parameters_from_context(
                videoStream->codecpar,
                codecContext) < 0)
            return false;

        videoStream->time_base = {1, 30};
        videoStream->avg_frame_rate = AVRational{30, 1};
        videoStream->r_frame_rate = AVRational{30, 1};

        if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(
                    &formatContext->pb,
                    filename,
                    AVIO_FLAG_WRITE) < 0)
                return false;
        }

        if (avformat_write_header(formatContext, nullptr) < 0)
            return false;

        frame = av_frame_alloc();

        if (!frame)
            return false;

        frame->format = codecContext->pix_fmt;
        frame->width = codecContext->width;
        frame->height = codecContext->height;

        if (av_frame_get_buffer(frame, 32) < 0)
            return false;

        packet = av_packet_alloc();

        if (!packet)
            return false;

        frameNumber = 0;
        recordingStart = std::chrono::steady_clock::now();
        recording = true;

        return true;
    }

    void encodeFrame(const uint8_t *y, const uint8_t *u, const uint8_t *v) {
        if (!recording)
            return;

        if (av_frame_make_writable(frame) < 0)
            return;

        for (int row = 0; row < 480; row++) {
            std::memcpy(frame->data[0] + row * frame->linesize[0], y + row * 640, 640);
        }

        for (int row = 0; row < 240; row++) {
            std::memcpy(frame->data[1] + row * frame->linesize[1], u + row * 320, 320);
            std::memcpy(frame->data[2] + row * frame->linesize[2], v + row * 320, 320);
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - recordingStart).count();

        frame->pts = av_rescale_q(elapsed, AVRational{1,1000000}, codecContext->time_base);

        if (avcodec_send_frame(codecContext, frame) < 0)
            return;

        while (avcodec_receive_packet(codecContext, packet) == 0) {
            av_packet_rescale_ts(packet, codecContext->time_base, videoStream->time_base);
            packet->stream_index = videoStream->index;
            av_interleaved_write_frame(formatContext, packet);
            av_packet_unref(packet);
        }
    }

    void stop() {
        if (!recording)
            return;

        avcodec_send_frame(codecContext, nullptr);

        while (avcodec_receive_packet(codecContext, packet) == 0) {
            av_packet_rescale_ts(
                packet,
                codecContext->time_base,
                videoStream->time_base
            );

            packet->stream_index = videoStream->index;
            av_interleaved_write_frame(formatContext, packet);
            av_packet_unref(packet);
        }

        av_write_trailer(formatContext);

        if (packet)
            av_packet_free(&packet);

        if (frame)
            av_frame_free(&frame);

        if (codecContext)
            avcodec_free_context(&codecContext);

        if (formatContext) {
            if (!(formatContext->oformat->flags & AVFMT_NOFILE))
                avio_closep(&formatContext->pb);

            avformat_free_context(formatContext);
        }

        formatContext = nullptr;
        codecContext = nullptr;
        videoStream = nullptr;
        frame = nullptr;
        packet = nullptr;

        recording = false;
        frameNumber = 0;
    }

    ~VideoRecorder() {
        stop();
    }
};

class CameraApp {
public:
    Camera *camera = nullptr;
    Stream *stream = nullptr;
    VideoRecorder recorder;

    std::vector<uint8_t> rgb;
    std::vector<MappedBuffer> mappedBuffers;

    YuvFrame yuvFrames[2];

    int latestFrame = -1;
    int processingFrame = -1;

    std::mutex frameMutex;

    MappedBuffer *findMappedBuffer(FrameBuffer *buffer) {
        for (auto &mapped : mappedBuffers) {
            if (mapped.buffer == buffer)
                return &mapped;
        }
        return nullptr;
    }

    void requestComplete(Request *request) {
        if (request->status() == Request::RequestCancelled) return;

        auto it = request->buffers().find(stream);

        if (it == request->buffers().end()) return;

        FrameBuffer *buffer = it->second;
        MappedBuffer *mapped = findMappedBuffer(buffer);

        if (!mapped || mapped->planes.size() < 3) return;

        int writeFrame;

        {
            std::lock_guard<std::mutex> lock(frameMutex);

            if (processingFrame == 0)
                writeFrame = 1;
            else
                writeFrame = 0;

            std::memcpy(
                yuvFrames[writeFrame].y.data(),
                mapped->planes[0].data,
                640 * 480
            );

            std::memcpy(
                yuvFrames[writeFrame].u.data(),
                mapped->planes[1].data,
                320 * 240
            );

            std::memcpy(
                yuvFrames[writeFrame].v.data(),
                mapped->planes[2].data,
                320 * 240
            );

            latestFrame = writeFrame;
        }

        request->reuse(Request::ReuseBuffers);
        camera->queueRequest(request);
    }
};

void yuv420_to_rgb(
    const uint8_t *y,
    const uint8_t *u,
    const uint8_t *v,
    uint8_t *rgb,
    int width,
    int height
)

{
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int Y = y[j * width + i];
            int U = u[(j / 2) * 320 + (i / 2)] - 128;
            int V = v[(j / 2) * 320 + (i / 2)] - 128;

            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            R = std::clamp(R, 0, 255);
            G = std::clamp(G, 0, 255);
            B = std::clamp(B, 0, 255);

            int p = (j * width + i) * 4;

            rgb[p + 0] = B;
            rgb[p + 1] = G;
            rgb[p + 2] = R;
            rgb[p + 3] = 0;
        }
    }
}

void takePhoto(const uint8_t *rgb, int width, int height) {
    time_t now = std::time(nullptr);
    tm *localTime = std::localtime(&now);

    char baseFilename[128];

    std::strftime(baseFilename, sizeof(baseFilename), "/home/antam/Pictures/photo_%Y-%m-%d_%H-%M-%S", localTime);

    char filename[160];

    std::snprintf(filename, sizeof(filename), "%s.jpg", baseFilename);

    int counter = 1;

    FILE *testFile;

    while ((testFile = std::fopen(filename, "r")) != nullptr) {
      std::fclose(testFile);

      std::snprintf(filename, sizeof(filename), "%s_%d.jpg", baseFilename, counter);
    }

    counter++;

    FILE *file = std::fopen(filename, "wb");

    if (!file) return;

    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    jpeg_stdio_dest(&cinfo, file);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> row(width * 3);

    while (cinfo.next_scanline < cinfo.image_height) {
        int y = cinfo.next_scanline;

        for (int x = 0; x < width; x++) {
            int p = (y * width + x) * 4;

            row[x * 3 + 0] = rgb[p + 2];
            row[x * 3 + 1] = rgb[p + 1];
            row[x * 3 + 2] = rgb[p + 0];
        }

        JSAMPROW rowPointer = row.data();

        jpeg_write_scanlines(&cinfo, &rowPointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    std::fclose(file);
}

int main() {
    display = XOpenDisplay(nullptr);

    if (!display)
        return 1;

    int screen = DefaultScreen(display);

    window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100, 640, 480, 1,
        BlackPixel(display, screen),
        BlackPixel(display, screen)
    );

    XStoreName(display, window, "Pi Camera");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask );
    XMapWindow(display, window);

    gc = XCreateGC(display, window, 0, nullptr);

    pixmap[0] = XCreatePixmap(display, window, 640, 480, DefaultDepth(display, screen));
    pixmap[1] = XCreatePixmap(display, window, 640, 480, DefaultDepth(display, screen));

    image = XCreateImage(
        display,
        DefaultVisual(display, screen),
        DefaultDepth(display, screen),
        ZPixmap, 0, nullptr,
        640, 480, 32, 0
    );

    image->data = static_cast<char *>(
        std::malloc(image->bytes_per_line * image->height));

    if (!image->data)
        return 1;

    CameraManager manager;

    if (manager.start() != 0)
        return 1;

    if (manager.cameras().empty())
        return 1;

    auto camera = manager.cameras()[0];
    CameraApp app;
    app.camera = camera.get();

    if (camera->acquire() != 0)
        return 1;

    auto config = camera->generateConfiguration({ StreamRole::Viewfinder });

    if (!config)
        return 1;

    auto &streamConfig = config->at(0);

    streamConfig.size.width = 640;
    streamConfig.size.height = 480;
    streamConfig.pixelFormat = formats::YUV420;

    if (config->validate() == CameraConfiguration::Invalid)
        return 1;

    if (camera->configure(config.get()) != 0)
        return 1;

    Stream *stream = streamConfig.stream();
    app.stream = stream;
    FrameBufferAllocator allocator(camera);

    app.rgb.resize(640 * 480 * 4);

    if (allocator.allocate(stream) < 0)
        return 1;

    const auto &buffers = allocator.buffers(stream);
    long pageSize = sysconf(_SC_PAGESIZE);

    for (const auto &buffer : buffers) {
        MappedBuffer mapped;
        mapped.buffer = buffer.get();

        for (const auto &plane : buffer->planes()) {
            off_t pageOffset = plane.offset & ~(pageSize - 1);
            size_t offsetInPage = plane.offset - pageOffset;
            size_t mapLength = plane.length + offsetInPage;

            void *memory = mmap(nullptr, mapLength, PROT_READ, MAP_SHARED, plane.fd.get(), pageOffset);

            if (memory == MAP_FAILED) {
                perror("mmap");
                return 1;
            }

            MappedPlane mappedPlane;
            mappedPlane.mapAddress = memory;
            mappedPlane.data = static_cast<uint8_t *>(memory) + offsetInPage;
            mappedPlane.mapLength = mapLength;
            mapped.planes.push_back(mappedPlane);
        }

        app.mappedBuffers.push_back(std::move(mapped));
    }

    std::vector<std::unique_ptr<Request>> requests;

    for (const auto &buffer : buffers) {
        auto request = camera->createRequest();

        if (!request) continue;
        if (request->addBuffer(stream, buffer.get()) < 0) continue;

        requests.push_back(std::move(request));
    }

    camera->requestCompleted.connect(&app, &CameraApp::requestComplete);

    ControlList controls(camera->controls());
    controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({33333, 33333}));

    if (camera->start(&controls) != 0)
        return 1;

    for (auto &request : requests)
        camera->queueRequest(request.get());

    bool running = true;

    while (running) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);

                if (key == XK_Escape)
                    running = false;

                if (key == XK_p || key == XK_P) {
                    takePhoto(app.rgb.data(), 640, 480);
                }

                if (key == XK_v || key == XK_V) {
                    if (!app.recorder.recording) {
                        app.recorder.start();
                    } else {
                        app.recorder.stop();
                    }
                }
            }
        }

        int frame = -1;

        {
            std::lock_guard<std::mutex> lock(app.frameMutex);

            if (app.latestFrame != -1) {
                frame = app.latestFrame;
                app.latestFrame = -1;
                app.processingFrame = frame;
            }
        }

        if (frame != -1) {
            const uint8_t *y = app.yuvFrames[frame].y.data();
            const uint8_t *u = app.yuvFrames[frame].u.data();
            const uint8_t *v = app.yuvFrames[frame].v.data();

            if (app.recorder.recording) {
                app.recorder.encodeFrame(y, u, v);
            }

            yuv420_to_rgb(y, u, v, app.rgb.data(), 640, 480);

            std::memcpy(image->data, app.rgb.data(), app.rgb.size());

            XPutImage(display, pixmap[currentPixmap], gc, image, 0, 0, 0, 0, 640, 480);

            if (app.recorder.recording) {
              auto now = std::chrono::steady_clock::now();
              auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - app.recorder.blinkTimer).count();

              if (elapsed >= 500) {
                app.recorder.dotVisible = !app.recorder.dotVisible;
                app.recorder.blinkTimer = now;
              }

              if (app.recorder.dotVisible) {
                XSetForeground(display, gc, 0xFF0000);
                XFillArc (display, pixmap[currentPixmap], gc, 35, 35, 40, 40, 0, 360 * 64);
              }
            }

            XPresentPixmap(display, window, pixmap[currentPixmap], 0, None, None, 0, 0, None, None, None, 0, 0, 0, 0, nullptr, 0);
            XFlush(display);

            currentPixmap ^= 1;

            {
                std::lock_guard<std::mutex> lock(app.frameMutex);
                app.processingFrame = -1;
            }
        }

        usleep(1000);
    }

    if (app.recorder.recording)
        app.recorder.stop();

    camera->stop();
    requests.clear();

    for (auto &mapped : app.mappedBuffers) {
        for (auto &plane : mapped.planes) {
            munmap(plane.mapAddress, plane.mapLength);
        }
    }

    app.mappedBuffers.clear();
    allocator.free(stream);
    camera->release();
    manager.stop();
    XFreePixmap(display, pixmap[0]);
    XFreePixmap(display, pixmap[1]);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
