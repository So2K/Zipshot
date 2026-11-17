// Memory-only API benchmark. No screenshot pixels are written or logged.
#include "../../src/capture.cpp"
#include <cstdio>
void Log(const wchar_t*, ...) {}
int wmain(int argc, wchar_t** argv) {
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int warm=_wtoi(argv[1]), count=_wtoi(argv[2]);
    RECT r=VirtualScreen();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    printf("{\"engine\":\"Zipshot CaptureRect\",\"x\":%ld,\"y\":%ld,\"width\":%ld,\"height\":%ld,\"cursor\":false,\"ms\":[",r.left,r.top,r.right-r.left,r.bottom-r.top);
    for(int i=-warm;i<count;i++) {
        LARGE_INTEGER a,b; QueryPerformanceCounter(&a);
        Image img=CaptureRect(r,false);
        QueryPerformanceCounter(&b);
        if(img.empty() || img.w != r.right-r.left || img.h != r.bottom-r.top) return 3;
        // Deliberately do not inspect, persist or publish screen content.
        if(i>=0) printf("%s%.6f",i ? "," : "",(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart);
    }
    printf("]}\n");
    return 0;
}
