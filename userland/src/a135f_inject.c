/*
 * liba135f.inject.dylib: put into backboardd by the kernel's exec_inject (boot-arg a135f_gl=1, patch
 * 0028; DYLD_INSERT_LIBRARIES). When the Mali's service is there it loads the OpenGL ES library
 * (liba135f.gl.dylib: src/eagl.c on Mesa's panfrost) which puts itself behind EAGLContext, and
 * CoreAnimation, finding no Metal device, draws with its OpenGL ES renderer on the GPU instead of its
 * CPU one. Without the service, or with boot-arg a135f_gl=0 given after it, nothing is loaded and the
 * CPU renderer stays. This needs no addresses in the dyld shared cache.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>

#define A135F_GL "/usr/local/lib/a135f/liba135f.gl.dylib"

static void
say(const char *m, const char *a)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "a135f: %s%s\n", m, a ? a : "");

	write(2, line, (size_t)n);
}

__attribute__((constructor))
static void
a135f_init(void)
{
	const char *name = getprogname();
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	io_service_t gpu;
	void *library;

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		for (const char *a = args; (a = strstr(a, "a135f_gl=")) != NULL; a += 9) {
			if ((a == args || a[-1] == ' ') && a[9] == '0') {
				say("a135f_gl=0: CoreAnimation keeps its CPU renderer", NULL);
				return;
			}
		}
	}
	if (access(A135F_GL, R_OK) != 0) {
		say("no OpenGL ES library: ", A135F_GL);
		return;
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("A135FMali"));
	if (gpu == 0) {
		say("no GPU service: CoreAnimation keeps its CPU renderer", NULL);
		return;
	}
	IOObjectRelease(gpu);
	if ((library = dlopen(A135F_GL, RTLD_NOW)) == NULL) {
		say("dlopen: ", dlerror());
		return;
	}
	say("OpenGL ES library loaded into backboardd", NULL);
}
