#include <libcamera/camera_manager.h>
#include <libcamera/camera.h>
#include <libcamera/stream.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/framebuffer.h>
#include <libcamera/request.h>
#include <libcamera/formats.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xpresent.h>
#include <sys/mman.h>

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

class CameraApp {
public:
    Camera *camera = nullptr;
    Stream *stream = nullptr;

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

            std::memcpy(yuvFrames[writeFrame].y.data(), mapped->planes[0].data, 640 * 480);
            std::memcpy(yuvFrames[writeFrame].u.data(), mapped->planes[1].data, 320 * 240);
            std::memcpy(yuvFrames[writeFrame].v.data(), mapped->planes[2].data, 320 * 240);

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

  char filename[64];

  std::strftime(filename, sizeof(filename), "/home/antam/Pictures/photo_%Y-%m-%d_%H_%M-%S.jpg", localTime);

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

            void *memory = mmap(
                nullptr,
                mapLength,
                PROT_READ,
                MAP_SHARED,
                plane.fd.get(),
                pageOffset
            );

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

    if (camera->start() != 0)
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

                if (key == XK_Escape) running = false;
                if (key == XK_p || key == XK_P) {
                    takePhoto(app.rgb.data(), 640, 480);
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
            yuv420_to_rgb(
                app.yuvFrames[frame].y.data(),
                app.yuvFrames[frame].u.data(),
                app.yuvFrames[frame].v.data(),
                app.rgb.data(),
                640, 480);

            std::memcpy(image->data, app.rgb.data(), app.rgb.size());

            XPutImage(display, pixmap[currentPixmap], gc, image, 0, 0, 0, 0, 640, 480);
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
