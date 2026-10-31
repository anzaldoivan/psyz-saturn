#include <psyz.h>
#include <libgpu.h>
#include <libetc.h>

#define OTSIZE 8
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

typedef struct DB {
    DRAWENV draw;
    DISPENV disp;
    OT_TYPE ot[OTSIZE];
    POLY_F3 f3[2];
    POLY_F4 f4[2];
} DB;

static DB db[2];

static void AddShapes(DB* d, int frame) {
    int i;
    int x = frame % 128;

    ClearOTag(d->ot, OTSIZE);
    for (i = 0; i < 2; i++) {
        SetPolyF3(&d->f3[i]);
        setXY3(&d->f3[i], 32 + x, 40 + i * 100, 96 + x, 72 + i * 100, 32 + x,
               104 + i * 100);
        setRGB0(&d->f3[i], 255, i * 255, 0);
        AddPrim(&d->ot[i], &d->f3[i]);

        SetPolyF4(&d->f4[i]);
        setXY4(&d->f4[i], 160, 40 + i * 100, 288, 40 + i * 100, 160,
               104 + i * 100, 288, 104 + i * 100);
        setRGB0(&d->f4[i], 0, 128, 255 - i * 128);
        AddPrim(&d->ot[2 + i], &d->f4[i]);
    }
}

int main(void) {
    int i;
    int frame;

    ResetGraph(0);
    for (i = 0; i < 2; i++) {
        SetDefDrawEnv(
            &db[i].draw, 0, i * SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
        SetDefDispEnv(&db[i].disp, 0, (1 - i) * SCREEN_HEIGHT, SCREEN_WIDTH,
                      SCREEN_HEIGHT);
        db[i].draw.isbg = 1;
        setRGB0(&db[i].draw, 0, 0, 64);
    }
    SetDispMask(1);

    for (frame = 0;; frame++) {
        DB* d = &db[frame & 1];
        AddShapes(d, frame);
        DrawSync(0);
        VSync(0);
        PutDrawEnv(&d->draw);
        PutDispEnv(&d->disp);
        DrawOTag(d->ot);
    }
    return 0;
}
