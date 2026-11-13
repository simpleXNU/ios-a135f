/*
 * The compiler library, loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU, and a device that refuses to be created at all takes
 * Metal's whole discovery down with it (the comment at the top of src/a135fmetal.m).
 */
#include "a13m.h"

#include <dlfcn.h>
#include <mutex>

namespace {

constexpr const char *library_path = "/usr/local/lib/a135f/liba135f.air.dylib";

struct Call {
	const char *name;
	void **slot;
	bool required;
};

struct a13m_air calls;
bool loaded;

template <typename F>
Call
call(const char *name, F *slot, bool required)
{
	return Call{ name, reinterpret_cast<void **>(slot), required };
}

void
load()
{
	void *handle = dlopen(library_path, RTLD_NOW);

	if (handle == nullptr) {
		a13m_print("no compiler library: %s\n", dlerror());
		return;
	}

	struct a13m_air &c = calls;
	const Call table[] = {
		call("a13a_device_open", &c.device_open, true),
		call("a13a_device_close", &c.device_close, false),
		call("a13a_device_name", &c.device_name, false),
		call("a13a_buffer_create", &c.buffer_create, false),
		call("a13a_buffer_map", &c.buffer_map, false),
		call("a13a_buffer_size", &c.buffer_size, false),
		call("a13a_buffer_destroy", &c.buffer_destroy, false),
		call("a13a_compute_create", &c.compute_create, true),
		call("a13a_compute_destroy", &c.compute_destroy, false),
		call("a13a_texture_create", &c.texture_create, true),
		call("a13a_texture_write", &c.texture_write, false),
		call("a13a_texture_read", &c.texture_read, false),
		call("a13a_texture_copy", &c.texture_copy, false),
		call("a13a_buffer_copy", &c.buffer_copy, false),
		call("a13a_buffer_fill", &c.buffer_fill, false),
		call("a13a_texture_mipmaps", &c.texture_mipmaps, false),
		call("a13a_render_clear", &c.render_clear, false),
		call("a13a_depth_read", &c.depth_read, false),
		call("a13a_texture_destroy", &c.texture_destroy, false),
		call("a13a_sampler_create", &c.sampler_create, true),
		call("a13a_sampler_destroy", &c.sampler_destroy, false),
		call("a13a_dispatch", &c.dispatch, true),
		/* argument buffers, drawing and depth: looked up, not required -- the plug-in checks
		 * for them before it answers an encoder, a pipeline or a depth state */
		call("a13a_dispatch_buffers", &c.dispatch_buffers, false),
		call("a13a_module_argbufs", &c.module_argbufs, false),
		call("a13a_argbuf_encode_buffer", &c.argbuf_encode_buffer, false),
		call("a13a_argbuf_encode_texture", &c.argbuf_encode_texture, false),
		call("a13a_argbuf_encode_sampler", &c.argbuf_encode_sampler, false),
		call("a13a_argbuf_encoded_members", &c.argbuf_encoded_members, false),
		call("a13a_render_constants", &c.render_constants, false),
		call("a13a_render_create", &c.render_create, false),
		call("a13a_render_destroy", &c.render_destroy, false),
		call("a13a_render_draw", &c.render_draw, false),
		call("a13a_depth_stencil_create", &c.depth_stencil_create, false),
		call("a13a_depth_stencil_destroy", &c.depth_stencil_destroy, false),
		call("a13a_finish", &c.finish, false),
		call("a13a_flush_seq", &c.flush_seq, false),
		call("a13a_wait_seq", &c.wait_seq, false),
		call("a13a_surface_seq", &c.surface_seq, false),
	};

	for (const Call &entry : table) {
		*entry.slot = dlsym(handle, entry.name);
		if (entry.required && *entry.slot == nullptr) {
			/* a13a_dispatch's arguments changed when textures were added to the contract, so
			 * an older library would be called with the wrong shape of argument and do
			 * something arbitrary: refusing it outright is the only safe reading */
			a13m_print("the compiler library has no %s, so it is older than this driver and "
				  "will not be used\n", entry.name);
			calls = {};
			dlclose(handle);
			return;
		}
	}
	c.handle = handle;
	loaded = true;
}

std::once_flag once;

} // namespace

extern "C" bool
a13m_air_available(void)
{
	std::call_once(once, load);
	return loaded;
}

extern "C" const struct a13m_air *
a13m_air(void)
{
	std::call_once(once, load);
	return &calls;
}
