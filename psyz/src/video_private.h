#ifndef PSYZ_VIDEO_PRIVATE_H
#define PSYZ_VIDEO_PRIVATE_H

typedef struct PsyzVideoCaptureState {
    int draw_x, draw_y, draw_w, draw_h;
    int display_x, display_y, display_w, display_h;
    int display_enabled;
} PsyzVideoCaptureState;

// Snapshot the page-selection state used by diagnostic captures.
int Psyz_VideoGetCaptureState(PsyzVideoCaptureState* state);

#endif
