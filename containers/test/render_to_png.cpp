// render_to_png: rasterize an HTML file to a PNG using the litehtml test
// container (canvas_ity software rasterizer + Ahem/Terminus fonts + lodepng).
//
// Usage: render_to_png <html_file> <width> <out_png> [height]
//   width   viewport / render width in px (required)
//   height  output image height in px; 0 or omitted = auto (document height)
//
// Exit code 0 on success, non-zero on failure. This is the C++ core of the
// reftest pixel pipeline: a reftest compares the PNG of a test file against the
// PNG of its reference, both produced by this same tool (so font and
// anti-aliasing differences cancel out -- the comparison is self-consistent).

#include "test_container.h"
#include "Bitmap.h"
#include "canvas_ity.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace litehtml;
using namespace canvas_ity;

static std::string read_text_file(const std::string& path)
{
    std::stringstream ss;
    std::ifstream     f(path, std::ios::binary);
    if(!f) return {};
    ss << f.rdbuf();
    return ss.str();
}

static std::string dir_of(const std::string& path)
{
    auto i = path.find_last_of("\\/");
    return i == std::string::npos ? std::string(".") : path.substr(0, i);
}

int main(int argc, char** argv)
{
    if(argc < 4)
    {
        fprintf(stderr, "usage: render_to_png <html_file> <width> <out_png> [height]\n");
        return 2;
    }
    const std::string html_file = argv[1];
    const int         width     = atoi(argv[2]);
    const std::string out_png   = argv[3];
    const int         req_h     = argc > 4 ? atoi(argv[4]) : 0;
    if(width <= 0)
    {
        fprintf(stderr, "invalid width\n");
        return 2;
    }

    std::string html = read_text_file(html_file);
    if(html.empty())
    {
        fprintf(stderr, "cannot read %s\n", html_file.c_str());
        return 3;
    }

    // The container needs a generous initial viewport height; the document grows
    // to its content during render() and we read the real height back below.
    test_container cont(width, req_h > 0 ? req_h : 4096, dir_of(html_file));
    auto           doc = document::createFromString(html.c_str(), &cont);
    if(!doc)
    {
        fprintf(stderr, "parse failed\n");
        return 4;
    }

    doc->render((pixel_t) width);

    int height = req_h > 0 ? req_h : (int) doc->height();
    if(height <= 0) height = 1;
    if(getenv("RTP_DEBUG"))
        fprintf(stderr, "[rtp] doc width=%d height=%d -> canvas %dx%d\n",
                (int) doc->width(), (int) doc->height(), width, height);

    canvas cvs(width, height);
    // Opaque white background, matching the default reftest canvas.
    cvs.set_color(fill_style, 1.f, 1.f, 1.f, 1.f);
    cvs.fill_rectangle(0.f, 0.f, (float) width, (float) height);

    position clip(0, 0, (pixel_t) width, (pixel_t) height);
    doc->draw((uint_ptr) &cvs, 0_px, 0_px, &clip);

    if(getenv("RTP_SELFTEST"))
    {
        // Draw a known red rectangle directly to verify the canvas pipeline.
        cvs.set_color(fill_style, 1.f, 0.f, 0.f, 1.f);
        cvs.fill_rectangle(10.f, 10.f, 50.f, 50.f);
    }

    Bitmap bmp(cvs);
    bmp.save(out_png);
    return 0;
}
