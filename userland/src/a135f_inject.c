/*
 * liba135f.inject.dylib: put into backboardd by the kernel's exec_inject (boot-arg a135f_gl=1, patch 0028;
 * DYLD_INSERT_LIBRARIES). It has two roads for CoreAnimation, taken in this order:
 *
 *  1. Metal, with boot-arg a135f_metal=1 as well (patch 0031 publishes IOAcceleratorES then): when the driver
 *     (A135FMetal.bundle in /System/Library/Extensions), QuartzCore's shaders in AIR (QC_METALLIB below) and the
 *     service are all there, the injector gives QuartzCore those shaders, has functions specialized by our
 *     compiler instead of Apple's (which dies on them), reports faults of backboardd into the kernel's log, and
 *     names the driver on the service -- before CoreAnimation first asks for a Metal device. If anything is
 *     missing it says what and goes on with the next road: a missing file must not become a crash loop.
 *  2. OpenGL ES, as before: when the Mali's service is there it loads liba135f.gl.dylib (src/eagl.c on Mesa's
 *     panfrost), which puts itself behind EAGLContext, and CoreAnimation, finding no Metal device, draws with
 *     its OpenGL ES renderer on the GPU instead of its CPU one. With boot-arg a135f_gl=0 given after it, nothing
 *     is loaded and the CPU renderer stays.
 *
 * What the Metal road does is found by name at run time (the Objective-C runtime), with no address in the dyld
 * shared cache, so it does not depend on which 16.x build the root is. What it does depend on is that the
 * QuartzCore default.metallib it hands out has the functions that build's QuartzCore asks for.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define A135F_GL "/usr/local/lib/a135f/liba135f.gl.dylib"
#define A135F_METAL_BUNDLE "/System/Library/Extensions/A135FMetal.bundle"
#define CACHE_BASE 0x180000000UL                /* where an arm64 shared cache is built to be, before the slide */

extern const void *_dyld_get_shared_cache_range(size_t *length);
static uintptr_t slide;

typedef kern_return_t (*IORegistryEntrySetCFProperties_fn)(
    io_registry_entry_t entry, CFTypeRef properties);

static IORegistryEntrySetCFProperties_fn load_set_cf_properties(void) {
    void *sym = dlsym(RTLD_DEFAULT, "IORegistryEntrySetCFProperties");
    if (!sym) {
        fprintf(stderr, "dlsym IORegistryEntrySetCFProperties: %s\n", dlerror());
    }
    return (IORegistryEntrySetCFProperties_fn)sym;
}

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static int
bootarg_is(const char *key, char value)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1, k = strlen(key);
	int found = -1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return -1;
	}
	for (const char *a = args; (a = strstr(a, key)) != NULL; a += k) {
		if ((a == args || a[-1] == ' ') && a[k] == '=') {
			found = a[k + 1] == value;      /* the last one given wins */
		}
	}
	return found;
}

/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("a135f: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * a135f_metal=1: CoreAnimation's Metal renderer, and the shaders it asks for. CA::OGL::new_metal_context
 * asserts that [QuartzCore's bundle URLForResource:@"default" withExtension:@"metallib"] is not nil
 * (ogl-metal.mm:3308), and this image has no such file: the iPhone X's firmware carries QuartzCore's
 * shaders only as default.pipelinelib, compiled for the A11. The same library in AIR -- which the
 * driver compiles for the Mali -- comes from the macOS QuartzCore (its first slice, cputype
 * 0x1000017), where the functions iOS 16.5 asks for (fixed_frag_lph_cph, std_vert0, narrow_blur_7_frag
 * and so on) carry the same names; build.sh puts it at QC_METALLIB. That one question about that one
 * resource is answered here, and every other is left to NSBundle.
 */
#define QC_METALLIB "/usr/local/share/a135f/QuartzCore-default.metallib"

static id (*orig_url_for_resource)(id bundle, SEL selector, id name, id extension);

static id
my_url_for_resource(id bundle, SEL selector, id name, id extension)
{
	BOOL (*same)(id, SEL, id) = (BOOL (*)(id, SEL, id))objc_msgSend;
	SEL equal = sel_registerName("isEqualToString:");
	id identifier = ((id (*)(id, SEL))objc_msgSend)(bundle, sel_registerName("bundleIdentifier"));

	if (name != nil && extension != nil && identifier != nil &&
	    same(identifier, equal, (id)CFSTR("com.apple.QuartzCore")) &&
	    same(name, equal, (id)CFSTR("default")) && same(extension, equal, (id)CFSTR("metallib")) &&
	    access(QC_METALLIB, R_OK) == 0) {
		static int told;

		if (!told) {
			told = 1;
			say("a135f: QuartzCore's default.metallib is " QC_METALLIB "\n");
		}
		return (id)CFAutorelease(CFURLCreateWithFileSystemPath(NULL, CFSTR(QC_METALLIB), kCFURLPOSIXPathStyle, false));
	}
	return orig_url_for_resource(bundle, selector, name, extension);
}

/*
 * The shaders are macOS 26's and the renderer is iOS 16.5's, and Apple's compiler service here
 * cannot specialize them: MTLCompilerService of 16.5 reads AIR 2.5 and dies on the library's 2.8
 * (45 crashes in one boot). So nothing is specialized by Apple at all. -newFunctionWithName:
 * constantValues: answers the plain function, and the values CoreAnimation set are kept on it
 * (associated, under the selector a135fFunctionConstants, which src/a135fmetal.m reads when
 * it builds a pipeline and hands to the compiler library: a13a_render_constants). A uchar3 is
 * kept as four bytes, the fourth 0: fc_color_opcodes0 is a uchar4 in the newer shader.
 */
typedef struct { unsigned long location, length; } NSRangeLike;

static void (*orig_set_constant_index)(id, SEL, const void *, unsigned long, unsigned long);
static void (*orig_set_constant_name)(id, SEL, const void *, unsigned long, id);
static void (*orig_set_constant_range)(id, SEL, const void *, unsigned long, NSRangeLike);

static size_t
constant_size(unsigned long type)
{
	struct { unsigned long first, elem; } groups[] = {
		{ 3, 4 } /* Float */, { 16, 2 } /* Half */, { 29, 4 } /* Int */, { 33, 4 } /* UInt */,
		{ 37, 2 } /* Short */, { 41, 2 } /* UShort */, { 45, 1 } /* Char */, { 49, 1 } /* UChar */,
		{ 53, 1 } /* Bool */,
	};

	for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		if (type >= groups[i].first && type < groups[i].first + 4) {
			return groups[i].elem * (type - groups[i].first + 1);
		}
	}
	return type == 81 || type == 85 ? 8 : 0;        /* Long, ULong */
}

static void
keep_constant(id values, id key, const void *value, unsigned long type)
{
	SEL slot = sel_registerName("a135fConstantValues");
	id kept = objc_getAssociatedObject(values, slot);
	uint64_t bits = 0;
	size_t size = constant_size(type);

	if (value == NULL || size == 0) {
		return;
	}
	memcpy(&bits, value, size < 8 ? size : 8);
	if (kept == nil) {
		kept = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), sel_registerName("new"));
		objc_setAssociatedObject(values, slot, kept, OBJC_ASSOCIATION_RETAIN);
		((void (*)(id, SEL))objc_msgSend)(kept, sel_registerName("release"));
	}
	((void (*)(id, SEL, id, id))objc_msgSend)(kept, sel_registerName("setObject:forKey:"),
	    ((id (*)(id, SEL, unsigned long long))objc_msgSend)((id)objc_getClass("NSNumber"),
	    sel_registerName("numberWithUnsignedLongLong:"), bits), key);
}

static id
constant_number(unsigned long n)
{
	return ((id (*)(id, SEL, unsigned long))objc_msgSend)((id)objc_getClass("NSNumber"),
	    sel_registerName("numberWithUnsignedLong:"), n);
}

static void
my_set_constant_index(id values, SEL selector, const void *value, unsigned long type, unsigned long index)
{
	keep_constant(values, constant_number(index), value, type);
	if (type == 51 && value != NULL) {
		uint8_t four[4] = { ((const uint8_t *)value)[0], ((const uint8_t *)value)[1],
		    ((const uint8_t *)value)[2], 0 };

		orig_set_constant_index(values, selector, four, 52, index);
		return;
	}
	orig_set_constant_index(values, selector, value, type, index);
}

static void
my_set_constant_name(id values, SEL selector, const void *value, unsigned long type, id name)
{
	keep_constant(values, name, value, type);
	if (type == 51 && value != NULL) {
		uint8_t four[4] = { ((const uint8_t *)value)[0], ((const uint8_t *)value)[1],
		    ((const uint8_t *)value)[2], 0 };

		orig_set_constant_name(values, selector, four, 52, name);
		return;
	}
	orig_set_constant_name(values, selector, value, type, name);
}

static void
my_set_constant_range(id values, SEL selector, const void *value, unsigned long type, NSRangeLike range)
{
	size_t size = constant_size(type);

	for (unsigned long i = 0; value != NULL && size != 0 && i < range.length; i++) {
		keep_constant(values, constant_number(range.location + i), (const uint8_t *)value + i * size, type);
	}
	orig_set_constant_range(values, selector, value, type, range);
}

/* the plain function, with the values CoreAnimation set kept on it by Metal's index */
static id
plain_function(id library, id name, id values)
{
	id function = ((id (*)(id, SEL, id))objc_msgSend)(library, sel_registerName("newFunctionWithName:"), name);
	id kept = values ? objc_getAssociatedObject(values, sel_registerName("a135fConstantValues")) : nil;

	if (function == nil || kept == nil) {
		return function;
	}
	id by_index = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), sel_registerName("new"));
	id constants = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("functionConstantsDictionary"));
	id keys = ((id (*)(id, SEL))objc_msgSend)(kept, sel_registerName("allKeys"));
	unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(keys, sel_registerName("count"));

	for (unsigned long i = 0; i < n; i++) {
		id key = ((id (*)(id, SEL, unsigned long))objc_msgSend)(keys, sel_registerName("objectAtIndex:"), i);
		id value = ((id (*)(id, SEL, id))objc_msgSend)(kept, sel_registerName("objectForKey:"), key);

		if (((BOOL (*)(id, SEL, Class))objc_msgSend)(key, sel_registerName("isKindOfClass:"), objc_getClass("NSString"))) {
			id constant = constants ? ((id (*)(id, SEL, id))objc_msgSend)(constants, sel_registerName("objectForKey:"), key) : nil;

			if (constant == nil) {
				continue;
			}
			key = constant_number(((unsigned long (*)(id, SEL))objc_msgSend)(constant, sel_registerName("index")));
		}
		((void (*)(id, SEL, id, id))objc_msgSend)(by_index, sel_registerName("setObject:forKey:"), value, key);
	}
	objc_setAssociatedObject(function, sel_registerName("a135fFunctionConstants"), by_index, OBJC_ASSOCIATION_RETAIN);
	((void (*)(id, SEL))objc_msgSend)(by_index, sel_registerName("release"));
	return function;
}

static id
my_new_function_values_error(id library, SEL selector, id name, id values, id *error)
{
	(void)selector;
	id function = plain_function(library, name, values);

	if (function == nil && error != NULL) {
		*error = nil;
	}
	return function;
}

static void
my_new_function_values_handler(id library, SEL selector, id name, id values, void (^handler)(id, id))
{
	(void)selector;
	id function = plain_function(library, name, values);

	handler(function, nil);
	if (function != nil) {
		((void (*)(id, SEL))objc_msgSend)(function, sel_registerName("release"));
	}
}

/* -newFunctionWithDescriptor:error:, which is what QuartzCore 16.5 sends: the descriptor carries
   the name and the constant values. Every class that implements it is hooked, its own
   implementation kept beside it. */
static struct { Class cls; IMP imp; } descriptor_originals[16];
static unsigned descriptor_original_count;

static id
my_new_function_descriptor(id library, SEL selector, id descriptor, id *error)
{
	id name = ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("name"));
	id values = ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("constantValues"));
	Class cls = object_getClass(library);

	if (name != nil && values != nil) {
		id function = plain_function(library, name, values);

		if (function != nil || error == NULL) {
			return function;
		}
	}
	for (; cls != Nil; cls = class_getSuperclass(cls)) {
		for (unsigned i = 0; i < descriptor_original_count; i++) {
			if (descriptor_originals[i].cls == cls) {
				return ((id (*)(id, SEL, id, id *))descriptor_originals[i].imp)(library, selector, descriptor, error);
			}
		}
	}
	return nil;
}

static int
hook_function_descriptors(void)
{
	SEL selector = sel_registerName("newFunctionWithDescriptor:error:");
	Protocol *protocol = objc_getProtocol("MTLLibrary");
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	int hooked = 0;

	for (unsigned i = 0; classes != NULL && i < count && descriptor_original_count < 16; i++) {
		unsigned n = 0;
		Method *methods;

		if (protocol != NULL && !class_conformsToProtocol(classes[i], protocol)) {
			continue;
		}
		methods = class_copyMethodList(classes[i], &n);
		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) == selector) {
				descriptor_originals[descriptor_original_count].cls = classes[i];
				descriptor_originals[descriptor_original_count++].imp =
				    method_setImplementation(methods[k], (IMP)my_new_function_descriptor);
				say("a135f: a135f_metal=1: -[%s newFunctionWithDescriptor:error:] specializes without Apple\n",
				    class_getName(classes[i]));
				hooked++;
			}
		}
		free(methods);
	}
	free(classes);
	return hooked;
}

static void
specialize_without_apple(void)
{
	Class values = objc_getClass("MTLFunctionConstantValuesInternal");
	Class library = objc_getClass("_MTLLibrary");
	Method m;
	int done = 0;

	if (values == Nil) {
		values = objc_getClass("MTLFunctionConstantValues");
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:atIndex:"))) != NULL) {
		orig_set_constant_index = (void (*)(id, SEL, const void *, unsigned long, unsigned long))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_index);
		done++;
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:withName:"))) != NULL) {
		orig_set_constant_name = (void (*)(id, SEL, const void *, unsigned long, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_name);
		done++;
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValues:type:withRange:"))) != NULL) {
		orig_set_constant_range = (void (*)(id, SEL, const void *, unsigned long, NSRangeLike))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_range);
		done++;
	}
	if (library != Nil &&
	    (m = class_getInstanceMethod(library, sel_registerName("newFunctionWithName:constantValues:error:"))) != NULL) {
		method_setImplementation(m, (IMP)my_new_function_values_error);
		done++;
	}
	if (library != Nil &&
	    (m = class_getInstanceMethod(library, sel_registerName("newFunctionWithName:constantValues:completionHandler:"))) != NULL) {
		method_setImplementation(m, (IMP)my_new_function_values_handler);
		done++;
	}
	done += hook_function_descriptors();
	say("a135f: a135f_metal=1: functions are specialized by the compiler library, not by Apple (%d hooks)\n", done);
}

/*
 * a135f_metal=1: where backboardd dies when no signal handler can say so -- a fault on a stack guard
 * page leaves the faulting thread no stack to run one on. A Mach exception port for the task
 * is served on a thread of its own instead: it reads the faulting thread's registers, names
 * pc, lr and the frame-pointer chain with dladdr (image and nearest symbol), keeps the lines
 * in the kernel's log (kern.a135f.say, "!" first, which the kernel says again every minute)
 * and answers KERN_FAILURE, so that the crash goes on exactly as before.
 */
#include <mach/exception_types.h>
#include <mach/thread_status.h>

static void
keep_line(const char *format, ...)
{
	char line[200] = "!";
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line + 1, sizeof(line) - 1, format, arguments);
	va_end(arguments);
	sysctlbyname("kern.a135f.say", NULL, NULL, line, strlen(line));
	say("%s\n", line + 1);
}

static void
name_address(const char *what, uintptr_t address)
{
	Dl_info info;

	if (address != 0 && dladdr((void *)address, &info) && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/');

		keep_line("bbd fault %s %#lx %s+%#lx %s+%#lx", what, address, image ? image + 1 : info.dli_fname,
		    address - (uintptr_t)info.dli_fbase, info.dli_sname ? info.dli_sname : "?",
		    info.dli_saddr ? address - (uintptr_t)info.dli_saddr : 0);
	} else {
		keep_line("bbd fault %s %#lx (no image)", what, address);
	}
}

#pragma pack(4)
struct exception_request {
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t thread;
	mach_msg_port_descriptor_t task;
	NDR_record_t ndr;
	exception_type_t exception;
	mach_msg_type_number_t code_count;
	int64_t code[2];
	char trailer[64];
};
struct exception_reply {
	mach_msg_header_t header;
	NDR_record_t ndr;
	kern_return_t result;
};
#pragma pack()

static void *
exception_thread(void *argument)
{
	mach_port_t port = (mach_port_t)(uintptr_t)argument;

	for (;;) {
		struct exception_request request;
		struct exception_reply reply;

		memset(&request, 0, sizeof(request));
		if (mach_msg(&request.header, MACH_RCV_MSG, 0, sizeof(request), port, MACH_MSG_TIMEOUT_NONE,
		    MACH_PORT_NULL) != KERN_SUCCESS) {
			continue;
		}
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		mach_port_t thread = request.thread.name;

		keep_line("bbd fault: exception %d code %#llx %#llx", request.exception,
		    (unsigned long long)request.code[0], (unsigned long long)request.code[1]);
		{
			/* the memory around the address that faulted: which mapping it was in, and how much
			 * of it there is -- a write past the end of a GPU buffer's mapping (pan_preload_fb_part)
			 * says here how long the mapping really was */
			vm_address_t at = (vm_address_t)request.code[1] - 0x200;
			for (int k = 0; k < 2; k++) {
				vm_address_t start = at;
				vm_size_t size = 0;
				vm_region_basic_info_data_64_t info;
				mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
				mach_port_t object = MACH_PORT_NULL;

				if (vm_region_64(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
				    (vm_region_info_t)&info, &n, &object) == KERN_SUCCESS) {
					keep_line("bbd fault region from %#llx: %#llx..%#llx prot %d/%d shared %d", (unsigned long long)at,
					    (unsigned long long)start, (unsigned long long)(start + size), info.protection,
					    info.max_protection, info.shared);
				} else {
					keep_line("bbd fault region from %#llx: none", (unsigned long long)at);
				}
				at = (vm_address_t)request.code[1];
			}
		}
		if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
			uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
			uintptr_t lr = (uintptr_t)arm_thread_state64_get_lr(state);
			uintptr_t sp = (uintptr_t)arm_thread_state64_get_sp(state);
			uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

			keep_line("bbd fault sp %#lx fp %#lx", sp, fp);
			name_address("pc", pc);
			name_address("lr", lr);
			for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
				uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

				if (ret == 0) {
					break;
				}
				name_address("frame", ret & 0x0000000fffffffffUL);
				fp = next > fp ? next : 0;
			}
		}
		memset(&reply, 0, sizeof(reply));
		reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.header.msgh_bits), 0);
		reply.header.msgh_remote_port = request.header.msgh_remote_port;
		reply.header.msgh_size = sizeof(reply);
		reply.header.msgh_id = request.header.msgh_id + 100;
		reply.ndr = NDR_record;
		reply.result = KERN_FAILURE;
		mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
		    MACH_PORT_NULL);
	}
	return NULL;
}

static void
watch_for_exceptions(void)
{
	mach_port_t port = MACH_PORT_NULL;
	pthread_t thread;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
	    task_set_exception_ports(mach_task_self(),
	    EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT |
	    EXC_MASK_GUARD | EXC_MASK_RESOURCE, port, (exception_behavior_t)(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES),
	    THREAD_STATE_NONE) != KERN_SUCCESS) {
		say("a135f: a135f_metal=1: no exception port\n");
		return;
	}
	pthread_create(&thread, NULL, exception_thread, (void *)(uintptr_t)port);
	say("a135f: a135f_metal=1: exceptions are reported (kept lines \"bbd fault\")\n");
}

static void
give_quartzcore_its_shaders(void)
{
	Method method = class_getInstanceMethod(objc_getClass("NSBundle"), sel_registerName("URLForResource:withExtension:"));

	if (method == NULL) {
		say("a135f: a135f_metal=1: no -[NSBundle URLForResource:withExtension:]; QuartzCore finds no shaders\n");
		return;
	}
	watch_for_faults();     /* where the Metal renderer dies, if it does */
	watch_for_exceptions();
	specialize_without_apple();
	/* The iPhone X's OLED panel has CoreAnimation correct for its overdrive with a look-up table
	   drawn by display_overdrive_lut_gen, a shader the macOS library has no copy of ("Metal failed
	   to load fragment function: function=display_overdrive_lut_gen_lph"). The panel here is an
	   LCD; CoreAnimation's own switch turns the correction off, read from the environment. */
	setenv("CA_DISABLE_DISPLAY_OVERDRIVE_FIX", "1", 1);
	{
		/* boot-arg ca_dirty=0: every frame drawn whole, not only its changed regions -- for telling
		 * whether the rectangles of older frames in the user's video (run 214) come from a surface
		 * that did not keep what CoreAnimation last drew into it */
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "ca_dirty=0") != NULL) {
			setenv("CA_DISABLE_DIRTY_REGIONS", "1", 1);
			keep_line("a135f: a135f_metal=1: CA_DISABLE_DIRTY_REGIONS, every frame drawn whole (ca_dirty=0)");
		}
	}
	{
		/* and what the driver says on its way (src/a135fmetal.m, a13_print, writes to standard
		   output, which launchd gives this job nowhere to go) */
		int console = open("/dev/console", O_WRONLY | O_NOCTTY);

		if (console >= 0) {
			dup2(console, 1);
			close(console);
		}
	}
	orig_url_for_resource = (id (*)(id, SEL, id, id))method_getImplementation(method);
	method_setImplementation(method, (IMP)my_url_for_resource);
	say("a135f: a135f_metal=1: QuartzCore's shaders come from %s (%s)\n", QC_METALLIB,
	    access(QC_METALLIB, R_OK) == 0 ? "there" : "MISSING");
}

/* Names the driver on the accelerator service (what src/metal_props.c does by hand). */
static int
name_the_driver(void)
{
	io_service_t accelerator = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
	const void *keys[2] = { CFSTR("MetalPluginName"), CFSTR("MetalPluginClassName") };
	const void *values[2] = { CFSTR("A135FMetal"), CFSTR("A135FMetalDevice") };
	IORegistryEntrySetCFProperties_fn setcfprops = load_set_cf_properties();
	if (!setcfprops) return 1;
	CFDictionaryRef properties;
	kern_return_t kr;

	if (accelerator == 0) {
		say("a135f: no IOAcceleratorES (boot-arg a135f_metal=1 and the kernel's patch 0031 make one)\n");
		return 0;
	}
	properties = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
//	kr = IORegistryEntrySetCFProperties(accelerator, properties);
	kr = setcfprops(accelerator, properties);

	CFRelease(properties);
	IOObjectRelease(accelerator);
	if (kr != KERN_SUCCESS) {
		say("a135f: the accelerator refused the driver's name (%#x)\n", kr);
		return 0;
	}
	return 1;
}

static int
metal_road(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (access(A135F_METAL_BUNDLE, R_OK) != 0) {
		say("a135f: a135f_metal=1 but no driver: %s\n", A135F_METAL_BUNDLE);
		return 0;
	}
	if (access(QC_METALLIB, R_OK) != 0) {
		say("a135f: a135f_metal=1 but no QuartzCore shaders: %s (see docs/METAL.md)\n", QC_METALLIB);
		return 0;
	}
	if (IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES")) == 0) {
		say("a135f: a135f_metal=1 but the kernel has no IOAcceleratorES (patch 0031?)\n");
		return 0;
	}
	slide = cache != NULL ? (uintptr_t)cache - CACHE_BASE : 0;
	give_quartzcore_its_shaders();
	if (!name_the_driver()) {
		return 0;       /* the hooks stay: they answer only questions that are not asked without the driver */
	}
	say("a135f: Metal: CoreAnimation may have the Metal device now\n");
	return 1;
}

__attribute__((constructor))
static void
a135f_init(void)
{
	const char *name = getprogname();
	io_service_t gpu;
	void *library;

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	if (bootarg_is("a135f_metal", '1') == 1 && metal_road()) {
		return;
	}
	if (bootarg_is("a135f_gl", '0') == 1) {
		say("a135f: a135f_gl=0: CoreAnimation keeps its CPU renderer\n");
		return;
	}
	if (access(A135F_GL, R_OK) != 0) {
		say("a135f: no OpenGL ES library: %s\n", A135F_GL);
		return;
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("A135FMali"));
	if (gpu == 0) {
		say("a135f: no GPU service: CoreAnimation keeps its CPU renderer\n");
		return;
	}
	IOObjectRelease(gpu);
	if ((library = dlopen(A135F_GL, RTLD_NOW)) == NULL) {
		say("a135f: dlopen: %s\n", dlerror());
		return;
	}
	say("a135f: OpenGL ES library loaded into backboardd\n");
}
