#ifdef __APPLE__
#define _XOPEN_SOURCE 600 // ucontext
#endif
#include <SDL3/SDL.h>
#include <psyz/log.h>
#include "../psyz/os.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <ucontext.h>
#endif

uint32_t Psyz_OsNowUs(void) { return (uint32_t)SDL_NS_TO_US(SDL_GetTicksNS()); }

uintptr_t Psyz_OsThreadSelf(void) {
    return (uintptr_t)SDL_GetCurrentThreadID();
}

void* Psyz_OsLockCreate(void) { return SDL_CreateMutex(); }
void Psyz_OsLock(void* lock) { SDL_LockMutex((SDL_Mutex*)lock); }
void Psyz_OsUnlock(void* lock) { SDL_UnlockMutex((SDL_Mutex*)lock); }

void* Psyz_OsBellCreate(void) { return SDL_CreateSemaphore(0); }

void Psyz_OsBellRing(void* bell) {
    SDL_Semaphore* sem = (SDL_Semaphore*)bell;
    if (SDL_GetSemaphoreValue(sem) == 0) {
        SDL_SignalSemaphore(sem);
    }
}

void Psyz_OsBellWait(void* bell, uint32_t timeout_us) {
    SDL_Semaphore* sem = (SDL_Semaphore*)bell;
    if (timeout_us == PSYZ_OS_FOREVER) {
        SDL_WaitSemaphore(sem);
        return;
    }
    // round up: waking early would spin until the deadline
    SDL_WaitSemaphoreTimeout(
        sem, (Sint32)(timeout_us / 1000 + (timeout_us % 1000 != 0)));
}

static int SDLCALL ThreadEntry(void* fn) {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    ((void (*)(void))fn)();
    return 0;
}

int Psyz_OsThreadStart(void (*fn)(void), const char* name) {
    SDL_Thread* thread = SDL_CreateThread(ThreadEntry, name, (void*)fn);
    if (!thread) {
        WARNF("cannot start thread %s: %s", name, SDL_GetError());
        return -1;
    }
    SDL_DetachThread(thread);
    return 0;
}

#ifdef _WIN32
static void WINAPI fiber_entry(void* fn) { ((void (*)(void))fn)(); }

void* Psyz_OsFiberSelf(void) {
    return IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
}

void* Psyz_OsFiberCreate(void (*fn)(void), unsigned int stack_size) {
    return CreateFiber(stack_size, fiber_entry, (void*)fn);
}

void Psyz_OsFiberSwitch(void* from, void* to) {
    (void)from;
    SwitchToFiber(to);
}

void Psyz_OsFiberDestroy(void* fiber) { DeleteFiber(fiber); }
#else
typedef struct {
    ucontext_t ctx;
    void* stack;
} fiber_t;

void* Psyz_OsFiberSelf(void) { return calloc(1, sizeof(fiber_t)); }

void* Psyz_OsFiberCreate(void (*fn)(void), unsigned int stack_size) {
    fiber_t* f = calloc(1, sizeof(fiber_t));
    if (!f || !(f->stack = malloc(stack_size)) || getcontext(&f->ctx)) {
        if (f) {
            free(f->stack);
        }
        free(f);
        return NULL;
    }
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, fn, 0);
    return f;
}

void Psyz_OsFiberSwitch(void* from, void* to) {
    swapcontext(&((fiber_t*)from)->ctx, &((fiber_t*)to)->ctx);
}

void Psyz_OsFiberDestroy(void* fiber) {
    if (fiber) {
        free(((fiber_t*)fiber)->stack);
        free(fiber);
    }
}
#endif
