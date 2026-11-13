/*
 * An argument encoder: a RECORDER. Metal's encoder writes 8-byte handles into a buffer and those are an Apple
 * GPU's descriptors, so this records (resource, Metal id) against (destination buffer, destination offset) in the
 * compiler library instead, and the draw binds every member the pipeline's shaders declare (docs/METAL.md 8.2).
 * The layout is the set's -- a shader's, or one the application described -- and the destination the buffer
 * -setArgumentBuffer:offset: named; which objects are the driver's own, and keeping the buffer alive, is the
 * plug-in's (A135FMetalArgumentEncoder).
 */
#include "a13m.h"

#include <stdio.h>
#include <string.h>

#include <new>

struct a13m_argenc {
	struct a13a_argbuf_layout layout;
	bool from_module;               /* the layout came from a shader, not descriptors */
	struct a13a_buffer *destination; /* the library's buffer -setArgumentBuffer:offset: named */
	uint8_t *contents;              /* ... and its memory */
	size_t offset;
	bool complained;
};

/*
 * A set the application describes rather than a shader declares: its members by Metal's index, each a buffer, a
 * texture or a sampler, 8 bytes each in Metal's encoding, bound wherever the application likes. What this port's
 * frontend cannot bind -- a value inside a set, an array of descriptors, indices with a gap or a repeat (Metal's
 * encodedLength is over the highest index) -- is said and refused. (MTLDataTypeTexture 58, Sampler 59, Pointer 60.)
 */
extern "C" bool
a13m_argenc_layout_of(const struct a13m_argument *arguments, unsigned long count, struct a13a_argbuf_layout *layout)
{
	const struct a13m_air *air = a13m_air();
	uint64_t seen = 0;

	if (air->argbuf_encode_buffer == nullptr || air->module_argbufs == nullptr) {
		a13m_print("this compiler library has no argument buffers in it, so "
			  "an encoder would record what no draw could read\n");
		return false;
	}
	if (count == 0 || count > A13A_MAX_ARGBUF_MEMBERS) {
		a13m_print("an argument encoder of %lu members: this driver records "
			  "1 to %u\n", count, A13A_MAX_ARGBUF_MEMBERS);
		return false;
	}
	*layout = a13a_argbuf_layout{};
	layout->metal_index = ~0u;      /* the application binds it where it likes */
	layout->alignment = 8;
	layout->member_count = (unsigned)count;
	layout->encoded_length = 8 * (unsigned)count;
	for (unsigned long i = 0; i < count; i++) {
		const unsigned long index = arguments[i].index, type = arguments[i].data_type;
		unsigned kind;

		switch (type) {
		case 60: kind = A13A_ARGBUF_BUFFER; break;
		case 58: kind = A13A_ARGBUF_TEXTURE; break;
		case 59: kind = A13A_ARGBUF_SAMPLER; break;
		default:
			a13m_print("member %lu of this argument encoder is "
				  "MTLDataType %lu, and this port binds a buffer, a texture or a "
				  "sampler member: a value inside a set is refused when the "
				  "shader is translated\n", index, type);
			return false;
		}
		if (arguments[i].array_length > 1) {
			a13m_print("member %lu of this argument encoder is an array "
				  "of %lu descriptors, and this port's frontend refuses a member "
				  "spanning more than one descriptor index\n", index, arguments[i].array_length);
			return false;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			a13m_print("this argument encoder has %lu members and one of "
				  "them is index %lu: Metal's encodedLength is over the highest "
				  "index, so this driver will not answer a length for indices "
				  "with a gap or a repeat in them\n", count, index);
			return false;
		}
		seen |= 1ull << index;
		layout->members[i].id = (unsigned)index;
		layout->members[i].kind = kind;
		layout->members[i].index = ~0u;
	}
	return true;
}

/* -[MTLFunction newArgumentEncoderWithBufferIndex:]: the sets the module declares, read by the frontend, and the one at
 * `index` -- or Metal's own answer, that there is no argument buffer there, with which indices there ARE, because an
 * application that asked for the wrong one has no other way to find out */
extern "C" bool
a13m_argenc_layout_for(struct a13a_device *device, const void *module, size_t module_size, const char *name,
		      unsigned long index, struct a13a_argbuf_layout *layout)
{
	const struct a13m_air *air = a13m_air();
	char why[512] = { 0 }, which[96] = "";
	struct a13a_argbuf_layout sets[8];
	unsigned count = 0;

	if (device == nullptr || air->module_argbufs == nullptr) {
		a13m_print("no compiler library, so the argument buffer layout of "
			  "this function cannot be read\n");
		return false;
	}
	if (air->module_argbufs(device, module, module_size, name, sets, (unsigned)(sizeof(sets) / sizeof(sets[0])),
				&count, why, sizeof(why)) != 0) {
		a13m_print("%s\n", why);     /* the frontend's own sentence: it names what it refused */
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index == (unsigned)index) {
			*layout = sets[i];
			return true;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	a13m_print("bufferIndex %lu does not identify an argument buffer: this function "
		  "declares %u%s\n", index, count, which);
	return false;
}

extern "C" struct a13m_argenc *
a13m_argenc_new(const struct a13a_argbuf_layout *layout, bool from_module)
{
	a13m_argenc *e = new (std::nothrow) a13m_argenc{};

	if (e != nullptr) {
		e->layout = *layout;
		e->from_module = from_module;
	}
	return e;
}

extern "C" void
a13m_argenc_free(struct a13m_argenc *e)
{
	delete e;
}

extern "C" const struct a13a_argbuf_layout *
a13m_argenc_layout(const struct a13m_argenc *e)
{
	return &e->layout;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's alignment, and so does
 * this -- an application that encodes at an unaligned offset has allocated its buffer from something other than
 * -encodedLength, and every record made afterwards would be keyed by an offset the draw will never look up.
 */
extern "C" bool
a13m_argenc_set_destination(struct a13m_argenc *e, bool given, struct a13a_buffer *buffer, void *contents, size_t offset)
{
	e->destination = nullptr;
	e->contents = nullptr;
	e->offset = 0;
	if (!given) {
		return true;
	}
	if (buffer == nullptr) {
		a13m_print("the argument buffer did not come from the compiler library, so nothing can be encoded into it\n");
		return false;
	}
	if (e->layout.alignment != 0 && offset % e->layout.alignment != 0) {
		a13m_print("an argument buffer offset of %lu is not a multiple of this encoder's alignment (%u), so the draw "
			  "would look for the encoding somewhere else\n", (unsigned long)offset, e->layout.alignment);
		return false;
	}
	e->destination = buffer;
	e->contents = (uint8_t *)contents;
	e->offset = offset;
	return true;
}

/* whether this encoder is in a state to record, said once per encoder for a missing destination: an id past the
 * set's own members is the mistake worth naming, because Metal would write a handle into the buffer and the shader
 * would read a descriptor that is not there */
extern "C" bool
a13m_argenc_ready(struct a13m_argenc *e, size_t index, const char *what)
{
	if (e->destination == nullptr) {
		if (!e->complained) {
			a13m_print("%s was encoded at id %lu with no argument buffer set (Metal's -setArgumentBuffer:offset: has "
				  "to name one first)\n", what, (unsigned long)index);
			e->complained = true;
		}
		return false;
	}
	if (index >= e->layout.member_count) {
		/* which of the two layouts this is matters to whoever reads the line: a shader's set is the shader's
		 * business, a set described with MTLArgumentDescriptors is the application's own */
		a13m_print("%s was encoded at id %lu of %s that declares %u member%s\n", what, (unsigned long)index,
			  e->from_module ? "a set a shader declares" : "an encoder this application described",
			  e->layout.member_count, e->layout.member_count == 1 ? "" : "s");
		return false;
	}
	return true;
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with the wrong one is refused
 * at the draw, by name, where the shader's own declaration is in hand. The library's sentence is passed through.
 */
extern "C" void
a13m_argenc_encode_buffer(struct a13m_argenc *e, size_t index, struct a13a_buffer *buffer, size_t offset)
{
	char why[512] = { 0 };

	if (a13m_air()->argbuf_encode_buffer(e->destination, e->offset, (unsigned)index, buffer, offset, why,
					    sizeof(why)) != 0) {
		a13m_print("%s\n", why);
	}
}

extern "C" void
a13m_argenc_encode_texture(struct a13m_argenc *e, size_t index, struct a13a_texture *texture)
{
	char why[512] = { 0 };

	if (a13m_air()->argbuf_encode_texture(e->destination, e->offset, (unsigned)index, texture, why, sizeof(why)) != 0) {
		a13m_print("%s\n", why);
	}
}

extern "C" void
a13m_argenc_encode_sampler(struct a13m_argenc *e, size_t index, struct a13a_sampler *sampler)
{
	char why[512] = { 0 };

	if (a13m_air()->argbuf_encode_sampler(e->destination, e->offset, (unsigned)index, sampler, why, sizeof(why)) != 0) {
		a13m_print("%s\n", why);
	}
}

/*
 * A VALUE member's place in the argument buffer -- an `int` or a `float4` inside the set -- which is what Metal
 * answers. Nothing of this driver reads those bytes (a shader reading a value out of a set is refused when it is
 * translated, docs/METAL.md 8.2.1), and that is said once; a real pointer is still the right answer, where NULL
 * would be a crash in the application's memcpy.
 */
extern "C" void *
a13m_argenc_constant_data(struct a13m_argenc *e, size_t index)
{
	static bool said;

	if (!a13m_argenc_ready(e, index, "constant data") || e->contents == nullptr) {
		return nullptr;
	}
	if (!said) {
		said = true;
		a13m_print("-constantDataAtIndex: hands back the argument buffer's own bytes, and nothing in this driver reads "
			  "them: a shader that reads a VALUE out of an argument buffer is refused when it is translated, because "
			  "every member this frontend binds is a descriptor\n");
	}
	return e->contents + e->offset + 8 * index;
}
