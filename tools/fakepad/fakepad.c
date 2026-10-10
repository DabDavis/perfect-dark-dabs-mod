// build: gcc -shared -fPIC -O1 -o fakepad.so fakepad.c -ldl -lSDL2
// use: LD_PRELOAD=$PWD/fakepad.so gdb ... -x lookprobe.py (PROBE_GE=1 sets GoldenEye mode); see CLAUDE-notes/ge-bean.md, "GoldenEye multiplayer's LOOK UP/DOWN"
// test-only LD_PRELOAD: one SDL virtual game controller in the process, whose
// axes are whatever gdb writes into g_TestAxis (no inferior calls needed)
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
volatile int g_TestAxis[6];
int g_TestAttached = -1;
int SDL_InitSubSystem(Uint32 flags)
{
	static int (*real)(Uint32);
	if (!real) real = dlsym(RTLD_NEXT, "SDL_InitSubSystem");
	int r = real(flags);
	if ((flags & SDL_INIT_GAMECONTROLLER) && g_TestAttached < 0) {
		g_TestAttached = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
	}
	return r;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, SDL_GameControllerAxis a)
{
	(void)c;
	return (a >= 0 && a < 6) ? (Sint16)g_TestAxis[a] : 0;
}
