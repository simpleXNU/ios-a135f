/*
 * A render pipeline built from the shape the plug-in read out of Metal's descriptor (its colour attachments and their
 * blend states, the depth and stencil formats, the vertex descriptor) and the modules it found: what is asked for
 * said, CoreAnimation's in-shader blends given source-over, one the warm-up thread built taken, else the recipe
 * recorded and the modules compiled -- one build at a time, on a stack of 16 MB, with the functions' constants.
 */
#include "a13m.h"

#include <string.h>

namespace {

bool
coreanimation_draw(const char *label)
{
	static const char prefix[] = "com.apple.coreanimation.draw.";

	return label != nullptr && strncmp(label, prefix, sizeof(prefix) - 1) == 0;
}

struct compile_call {
	struct a13a_device *device;
	const struct a13m_render_request *request;
	struct a13a_render_shape *shape;
	struct a13a_render_info *info;
	char *why;
	size_t why_size;
	struct a13a_render *render;
};

/* (set on the thread that compiles: a13a_render_constants is per thread) */
void
compile_main(void *context)
{
	compile_call *c = (compile_call *)context;
	const struct a13m_render_request *r = c->request;
	const struct a13m_air *air = a13m_air();

	if (air->render_constants != nullptr) {
		air->render_constants(r->vertex_constants, r->vertex_constant_count, r->fragment_constants,
				      r->fragment_constant_count);
	}
	/* No entry names: a module out of a .metallib has one entry point, and the compiler takes it. Named, it insists
	 * the two agree, which refuses an alias -- a second function-list entry over another's module, which is how
	 * QuartzCore's library here answers for the few functions iOS 16.5 asks for and the macOS library it comes from
	 * has not got (p3_clamp, display_overdrive_*, path_stroke_*: userland/build.sh, QC_METALLIB). */
	c->render = air->render_create(c->device, r->vertex, r->vertex_size, nullptr, r->fragment, r->fragment_size,
				       nullptr, c->shape, c->info, c->why, c->why_size);
}

} // namespace

extern "C" bool
a13m_render_prepare(const char *label, struct a13a_render_shape *shape, bool has_vertex_descriptor,
		   struct a13a_render **render, struct a13a_render_info *info)
{
	/* what it asks for, said before the source-over that CoreAnimation's in-shader blends are given, so that the
	 * lines say what was asked (src/metal/shape.cpp) */
	a13m_shape_say(label, shape, has_vertex_descriptor);
	a13m_xm_blend_apply(label, shape);
	if (coreanimation_draw(label) && a13m_warm_take(label, shape, render, info)) {
		if (shape->color_count > 0) {
			a13m_blend_result(info->blend_enabled_mask, info->color_write_masks);
		}
		return true;
	}
	return false;
}

extern "C" struct a13a_render *
a13m_render_compile(struct a13a_device *device, const char *label, const struct a13m_render_request *r,
		   struct a13a_render_shape *shape, struct a13a_render_info *info, char *why, size_t why_size)
{
	static unsigned long refused;
	compile_call call = { device, r, shape, info, why, why_size, nullptr };

	if (coreanimation_draw(label)) {
		a13m_recipe_record(label, shape, r->vertex_name, r->fragment_name, r->vertex_constants,
				  r->vertex_constant_count, r->fragment_constants, r->fragment_constant_count);
	}
	a13m_compile_lock();     /* (one build at a time: the warm-up thread builds too) */
	a13m_on_big_stack(compile_main, &call);
	a13m_compile_unlock();
	if (call.render == nullptr) {
		/*
		 * Printed as well as returned, and only the first, second, fourth... time, because the same specification
		 * is asked for over and over: the caller's NSError is the sentence and nothing more, and CoreAnimation
		 * discards it and retries. A refusal that reaches no log is one nobody can grep for -- one specification was
		 * declined 80 times over 42 lives of backboardd before the vertex layout existed, counted by hand.
		 */
		if ((++refused & (refused - 1)) == 0) {
			a13m_print("this render pipeline is refused: %s\n", why);
			if (refused > 1) {
				a13m_print("  ... and that is the %lu'th time\n", refused);
			}
		}
		return nullptr;
	}
	/* what the library made of it, in GALLIUM's bits: for any mask but All, None, green|blue and red|alpha these
	 * nibbles must DIFFER from the writeMask said above, the two enumerations numbering their channels in opposite
	 * directions */
	if (shape->color_count > 0) {
		a13m_blend_result(info->blend_enabled_mask, info->color_write_masks);
	}
	return call.render;
}
