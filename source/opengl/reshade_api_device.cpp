/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "dll_log.hpp"
#include "ini_file.hpp"
#include "reshade_api_device.hpp"
#include "reshade_api_type_convert.hpp"
#include <cassert>

static GLint get_rbo_param(GLuint id, GLenum param)
{
	GLint value = 0;
	if (gl3wProcs.gl.GetNamedRenderbufferParameteriv != nullptr)
	{
		glGetNamedRenderbufferParameteriv(id, param, &value);
	}
	else
	{
		GLint prev_binding = 0;
		glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_binding);
		glBindRenderbuffer(GL_RENDERBUFFER, id);
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &value);
		glBindRenderbuffer(GL_RENDERBUFFER, prev_binding);
	}
	return value;
}
static GLint get_buf_param(GLenum target, GLuint id, GLenum param)
{
	GLint value = 0;
	if (gl3wProcs.gl.GetNamedBufferParameteriv != nullptr)
	{
		glGetNamedBufferParameteriv(id, param, &value);
	}
	else
	{
		GLint prev_binding = 0;
		glGetIntegerv(reshade::opengl::get_binding_for_target(target), &prev_binding);
		glBindBuffer(target, id);
		glGetBufferParameteriv(target, param, &value);
		glBindBuffer(target, prev_binding);
	}
	return value;
}
static GLint get_tex_param(GLenum target, GLuint id, GLenum param)
{
	GLint value = 0;
	if (gl3wProcs.gl.GetTextureParameteriv != nullptr)
	{
		glGetTextureParameteriv(id, param, &value);
	}
	else
	{
		if (GL_TEXTURE == target)
			target = GL_TEXTURE_2D;

		GLint prev_binding = 0;
		glGetIntegerv(reshade::opengl::get_binding_for_target(target), &prev_binding);
		glBindTexture(target, id);
		glGetTexParameteriv(target, param, &value);
		glBindTexture(target, prev_binding);
	}
	return value;
}
static GLint get_tex_level_param(GLenum target, GLuint id, GLuint level, GLenum param)
{
	GLint value = 0;
	if (gl3wProcs.gl.GetTextureLevelParameteriv != nullptr)
	{
		glGetTextureLevelParameteriv(id, level, param, &value);
	}
	else
	{
		if (GL_TEXTURE == target)
			target = GL_TEXTURE_2D;

		GLint prev_binding = 0;
		glGetIntegerv(reshade::opengl::get_binding_for_target(target), &prev_binding);
		glBindTexture(target, id);
		glGetTexLevelParameteriv(target, level, param, &value);
		glBindTexture(target, prev_binding);
	}
	return value;
}
static GLint get_fbo_attachment_param(GLuint id, GLenum attachment, GLenum param)
{
	GLint value = 0;
	if (gl3wProcs.gl.GetNamedFramebufferAttachmentParameteriv != nullptr)
	{
		glGetNamedFramebufferAttachmentParameteriv(id, attachment, param, &value);
	}
	else
	{
		GLint prev_binding = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_binding);
		glBindFramebuffer(GL_FRAMEBUFFER, id);
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, param, &value);
		glBindFramebuffer(GL_FRAMEBUFFER, prev_binding);
	}
	return value;
}

reshade::opengl::device_impl::device_impl(HDC initial_hdc, HGLRC hglrc) :
	api_object_impl(hglrc)
{
	RECT window_rect = {};
	GetClientRect(WindowFromDC(initial_hdc), &window_rect);

	_default_fbo_width = window_rect.right - window_rect.left;
	_default_fbo_height = window_rect.bottom - window_rect.top;

	// The pixel format has to be the same for all device contexts used with this rendering context, so can cache information about it here
	// See https://docs.microsoft.com/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent
	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd) };
	DescribePixelFormat(initial_hdc, GetPixelFormat(initial_hdc), sizeof(pfd), &pfd);

	switch (pfd.cRedBits)
	{
	default:
	case  0: _default_color_format = GL_NONE;
		break;
	case  8: _default_color_format = GL_RGBA8;
		break;
	case 10: _default_color_format = GL_RGB10_A2;
		break;
	case 16: _default_color_format = GL_RGBA16F;
		break;
	}

	switch (pfd.cDepthBits)
	{
	default:
	case  0: _default_depth_format = GL_NONE; // No depth in this pixel format
		break;
	case 16: _default_depth_format = GL_DEPTH_COMPONENT16;
		break;
	case 24: _default_depth_format = pfd.cStencilBits ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24;
		break;
	case 32: _default_depth_format = pfd.cStencilBits ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32;
		break;
	}

	// Check for special extension to detect whether this is a compatibility context (https://www.khronos.org/opengl/wiki/OpenGL_Context#OpenGL_3.1_and_ARB_compatibility)
	GLint num_extensions = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &num_extensions);
	for (GLint i = 0; i < num_extensions; ++i)
	{
		const GLubyte *const extension = glGetStringi(GL_EXTENSIONS, i);
		if (std::strcmp(reinterpret_cast<const char *>(extension), "GL_ARB_compatibility") == 0)
		{
			_compatibility_context = true;
			break;
		}
	}

#ifndef NDEBUG
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDebugMessageCallback([](unsigned int /*source*/, unsigned int type, unsigned int /*id*/, unsigned int /*severity*/, int /*length*/, const char *message, const void */*userParam*/) {
		if (type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)
			OutputDebugStringA(message), OutputDebugStringA("\n");
	}, nullptr);
#endif

	// Some games (like Hot Wheels Velocity X) use fixed texture names, which can clash with the ones ReShade generates below, since most implementations will return values linearly
	// Reserve a configurable range of texture names in old OpenGL games (which will use a compatibility context) to work around this
	auto num_reserve_texture_names = _compatibility_context ? 512u : 0u;
	reshade::global_config().get("APP", "ReserveTextureNames", num_reserve_texture_names);
	_reserved_texture_names.resize(num_reserve_texture_names);
	if (!_reserved_texture_names.empty())
		glGenTextures(static_cast<GLsizei>(_reserved_texture_names.size()), _reserved_texture_names.data());

	// Generate push constants buffer name
	glGenBuffers(1, &_push_constants);

	// Generate copy framebuffers
	glGenFramebuffers(2, _copy_fbo);

	// Create mipmap generation program used in the 'generate_mipmaps' function
	{
		const GLchar *mipmap_shader[] = {
			"#version 430\n"
			"layout(binding = 0) uniform sampler2D src;\n"
			"layout(binding = 1) uniform writeonly image2D dest;\n"
			"layout(location = 0) uniform vec3 info;\n"
			"layout(local_size_x = 8, local_size_y = 8) in;\n"
			"void main()\n"
			"{\n"
			"	vec2 uv = info.xy * (vec2(gl_GlobalInvocationID.xy) + vec2(0.5));\n"
			"	imageStore(dest, ivec2(gl_GlobalInvocationID.xy), textureLod(src, uv, int(info.z)));\n"
			"}\n"
		};

		const GLuint mipmap_cs = glCreateShader(GL_COMPUTE_SHADER);
		glShaderSource(mipmap_cs, 1, mipmap_shader, 0);
		glCompileShader(mipmap_cs);

		_mipmap_program = glCreateProgram();
		glAttachShader(_mipmap_program, mipmap_cs);
		glLinkProgram(_mipmap_program);
		glDeleteShader(mipmap_cs);
	}

#if RESHADE_ADDON
	addon::load_addons();

	invoke_addon_event<addon_event::init_device>(this);
	invoke_addon_event<addon_event::init_command_queue>(this);

	// Communicate default state to add-ons
	invoke_addon_event<addon_event::begin_render_pass>(this, make_render_pass_handle(0));
#endif
}
reshade::opengl::device_impl::~device_impl()
{
#if RESHADE_ADDON
	invoke_addon_event<addon_event::destroy_command_queue>(this);
	invoke_addon_event<addon_event::destroy_device>(this);

	addon::unload_addons();
#endif

	// Destroy mipmap generation program
	glDeleteProgram(_mipmap_program);

	// Destroy framebuffers used in 'copy_resource' implementation
	glDeleteFramebuffers(2, _copy_fbo);

	// Destroy push constants buffer
	glDeleteBuffers(1, &_push_constants);

	// Free range of reserved texture names
	glDeleteTextures(static_cast<GLsizei>(_reserved_texture_names.size()), _reserved_texture_names.data());
}

bool reshade::opengl::device_impl::check_capability(api::device_caps capability) const
{
	GLint value;

	switch (capability)
	{
	case api::device_caps::compute_shader:
		return true; // OpenGL 4.3
	case api::device_caps::geometry_shader:
		return true; // OpenGL 3.2
	case api::device_caps::hull_and_domain_shader:
		return true; // OpenGL 4.0
	case api::device_caps::dual_src_blend:
		return true; // OpenGL 3.3
	case api::device_caps::independent_blend:
		// TODO
		return false;
	case api::device_caps::logic_op:
		return true; // OpenGL 1.1
	case api::device_caps::draw_instanced:
		return true; // OpenGL 3.1
	case api::device_caps::draw_or_dispatch_indirect:
		return true; // OpenGL 4.0
	case api::device_caps::fill_mode_non_solid:
	case api::device_caps::multi_viewport:
		return true;
	case api::device_caps::partial_push_constant_updates:
		return false;
	case api::device_caps::partial_push_descriptor_updates:
	case api::device_caps::sampler_compare:
		return true;
	case api::device_caps::sampler_anisotropic:
		glGetIntegerv(GL_TEXTURE_MAX_ANISOTROPY, &value); // Core in OpenGL 4.6
		return value > 1;
	case api::device_caps::sampler_with_resource_view:
	case api::device_caps::copy_buffer_region:
	case api::device_caps::copy_buffer_to_texture:
	case api::device_caps::blit:
	case api::device_caps::resolve_region:
		return true;
	case api::device_caps::copy_query_pool_results:
		return gl3wProcs.gl.GetQueryBufferObjectui64v != nullptr; // OpenGL 4.5
	default:
		return false;
	}
}
bool reshade::opengl::device_impl::check_format_support(api::format format, api::resource_usage usage) const
{
	const GLenum internal_format = convert_format(format);
	if (internal_format == GL_NONE)
		return false;

	GLint supported = GL_FALSE;
	glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_INTERNALFORMAT_SUPPORTED, 1, &supported);

	GLint supported_depth = GL_TRUE;
	GLint supported_stencil = GL_TRUE;
	if ((usage & api::resource_usage::depth_stencil) != api::resource_usage::undefined)
	{
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_DEPTH_RENDERABLE, 1, &supported_depth);
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_STENCIL_RENDERABLE, 1, &supported_stencil);
	}

	GLint supported_color_render = GL_TRUE;
	GLint supported_render_target = GL_CAVEAT_SUPPORT;
	if ((usage & api::resource_usage::render_target) != api::resource_usage::undefined)
	{
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_COLOR_RENDERABLE, 1, &supported_color_render);
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_FRAMEBUFFER_RENDERABLE, 1, &supported_render_target);
	}

	GLint supported_unordered_access_load = GL_CAVEAT_SUPPORT;
	GLint supported_unordered_access_store = GL_CAVEAT_SUPPORT;
	if ((usage & api::resource_usage::unordered_access) != api::resource_usage::undefined)
	{
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_SHADER_IMAGE_LOAD, 1, &supported_unordered_access_load);
		glGetInternalformativ(GL_TEXTURE_2D, internal_format, GL_SHADER_IMAGE_STORE, 1, &supported_unordered_access_store);
	}

	return supported && (supported_depth || supported_stencil) && (supported_color_render && supported_render_target) && (supported_unordered_access_load && supported_unordered_access_store);
}

bool reshade::opengl::device_impl::is_resource_handle_valid(api::resource handle) const
{
	const GLuint object = handle.handle & 0xFFFFFFFF;
	switch (handle.handle >> 40)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		return glIsBuffer(object) != GL_FALSE;
	case GL_TEXTURE:
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
		return glIsTexture(object) != GL_FALSE;
	case GL_RENDERBUFFER:
		return glIsRenderbuffer(object) != GL_FALSE;
	case GL_FRAMEBUFFER_DEFAULT:
		return (object != GL_DEPTH_STENCIL_ATTACHMENT && object != GL_DEPTH_ATTACHMENT && object != GL_STENCIL_ATTACHMENT) || _default_depth_format != GL_NONE;
	default:
		return false;
	}
}
bool reshade::opengl::device_impl::is_resource_view_handle_valid(api::resource_view handle) const
{
	return is_resource_handle_valid({ handle.handle });
}

bool reshade::opengl::device_impl::create_sampler(const api::sampler_desc &desc, api::sampler *out)
{
	GLuint object = 0;
	glGenSamplers(1, &object);

	GLenum min_filter = GL_NONE;
	GLenum mag_filter = GL_NONE;
	switch (desc.filter)
	{
	case api::filter_type::min_mag_mip_point:
	case api::filter_type::compare_min_mag_mip_point:
		min_filter = GL_NEAREST_MIPMAP_NEAREST;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_type::min_mag_point_mip_linear:
	case api::filter_type::compare_min_mag_point_mip_linear:
		min_filter = GL_NEAREST_MIPMAP_LINEAR;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_type::min_point_mag_linear_mip_point:
	case api::filter_type::compare_min_point_mag_linear_mip_point:
		min_filter = GL_NEAREST_MIPMAP_NEAREST;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_type::min_point_mag_mip_linear:
	case api::filter_type::compare_min_point_mag_mip_linear:
		min_filter = GL_NEAREST_MIPMAP_LINEAR;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_type::min_linear_mag_mip_point:
	case api::filter_type::compare_min_linear_mag_mip_point:
		min_filter = GL_LINEAR_MIPMAP_NEAREST;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_type::min_linear_mag_point_mip_linear:
	case api::filter_type::compare_min_linear_mag_point_mip_linear:
		min_filter = GL_LINEAR_MIPMAP_LINEAR;
		mag_filter = GL_NEAREST;
		break;
	case api::filter_type::min_mag_linear_mip_point:
	case api::filter_type::compare_min_mag_linear_mip_point:
		min_filter = GL_LINEAR_MIPMAP_NEAREST;
		mag_filter = GL_LINEAR;
		break;
	case api::filter_type::anisotropic:
	case api::filter_type::compare_anisotropic:
		glSamplerParameterf(object, GL_TEXTURE_MAX_ANISOTROPY, desc.max_anisotropy);
		[[fallthrough]];
	case api::filter_type::min_mag_mip_linear:
	case api::filter_type::compare_min_mag_mip_linear:
		min_filter = GL_LINEAR_MIPMAP_LINEAR;
		mag_filter = GL_LINEAR;
		break;
	}

	const auto convert_address_mode = [](api::texture_address_mode value) {
		switch (value)
		{
		default:
			return GL_NONE;
		case api::texture_address_mode::wrap:
			return GL_REPEAT;
		case api::texture_address_mode::mirror:
			return GL_MIRRORED_REPEAT;
		case api::texture_address_mode::clamp:
			return GL_CLAMP_TO_EDGE;
		case api::texture_address_mode::border:
			return GL_CLAMP_TO_BORDER;
		}
	};

	glSamplerParameteri(object, GL_TEXTURE_MIN_FILTER, min_filter);
	glSamplerParameteri(object, GL_TEXTURE_MAG_FILTER, mag_filter);
	glSamplerParameteri(object, GL_TEXTURE_WRAP_S, convert_address_mode(desc.address_u));
	glSamplerParameteri(object, GL_TEXTURE_WRAP_T, convert_address_mode(desc.address_v));
	glSamplerParameteri(object, GL_TEXTURE_WRAP_R, convert_address_mode(desc.address_w));
	glSamplerParameterf(object, GL_TEXTURE_LOD_BIAS, desc.mip_lod_bias);
	glSamplerParameteri(object, GL_TEXTURE_COMPARE_MODE, (static_cast<uint32_t>(desc.filter) & 0x80) != 0 ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
	glSamplerParameteri(object, GL_TEXTURE_COMPARE_FUNC, convert_compare_op(desc.compare_op));
	glSamplerParameterf(object, GL_TEXTURE_MIN_LOD, desc.min_lod);
	glSamplerParameterf(object, GL_TEXTURE_MAX_LOD, desc.max_lod);

	*out = { (static_cast<uint64_t>(GL_SAMPLER) << 40) | object };
	return true;
}
bool reshade::opengl::device_impl::create_resource(const api::resource_desc &desc, const api::subresource_data *initial_data, api::resource_usage, api::resource *out)
{
	GLenum target = GL_NONE;
	switch (desc.type)
	{
	case api::resource_type::buffer:
		switch (desc.usage & (api::resource_usage::index_buffer | api::resource_usage::vertex_buffer | api::resource_usage::constant_buffer))
		{
		case api::resource_usage::index_buffer:
			target = GL_ELEMENT_ARRAY_BUFFER;
			break;
		case api::resource_usage::vertex_buffer:
			target = GL_ARRAY_BUFFER;
			break;
		case api::resource_usage::constant_buffer:
			target = GL_UNIFORM_BUFFER;
			break;
		default:
			if (desc.heap == api::memory_heap::gpu_to_cpu)
			{
				target = GL_PIXEL_PACK_BUFFER;
				break;
			}
			if (desc.heap == api::memory_heap::cpu_to_gpu)
			{
				target = GL_PIXEL_UNPACK_BUFFER;
				break;
			}
			*out = { 0 };
			assert(false);
			return false;
		}
		break;
	case api::resource_type::texture_1d:
		target = desc.texture.depth_or_layers > 1 ? GL_TEXTURE_1D_ARRAY : GL_TEXTURE_1D;
		break;
	case api::resource_type::texture_2d:
		if ((desc.flags & api::resource_flags::cube_compatible) != api::resource_flags::cube_compatible)
			target = desc.texture.depth_or_layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
		else
			target = desc.texture.depth_or_layers > 6 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
		break;
	case api::resource_type::texture_3d:
		target = GL_TEXTURE_3D;
		break;
	default:
		*out = { 0 };
		assert(false);
		return false;
	}

	GLuint object = 0;
	GLuint prev_object = 0;
	glGetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_object));

	if (desc.type == api::resource_type::buffer)
	{
		if (out != _current_event_handle)
		{
			glGenBuffers(1, &object);
		}
		else
		{
			assert((out->handle >> 40) == target);
			object = out->handle & 0xFFFFFFFF;
		}

		glBindBuffer(target, object);

		GLbitfield usage_flags = GL_NONE;
		convert_memory_heap_to_flags(desc, usage_flags);

		assert(desc.buffer.size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()));
		glBufferStorage(target, static_cast<GLsizeiptr>(desc.buffer.size), nullptr, usage_flags);

		if (initial_data != nullptr)
		{
			upload_buffer_region(initial_data->data, make_resource_handle(target, object), 0, desc.buffer.size);
		}

		glBindBuffer(target, prev_object);
	}
	else
	{
		const GLenum internal_format = convert_format(desc.texture.format);
		if (internal_format == GL_NONE)
		{
			*out = { 0 };
			return false;
		}

		if (out != _current_event_handle)
		{
			glGenTextures(1, &object);
		}
		else
		{
			assert((out->handle >> 40) == target);
			object = out->handle & 0xFFFFFFFF;
		}

		glBindTexture(target, object);

		GLuint depth_or_layers = desc.texture.depth_or_layers;
		switch (target)
		{
		case GL_TEXTURE_1D:
			glTexStorage1D(target, desc.texture.levels, internal_format, desc.texture.width);
			break;
		case GL_TEXTURE_1D_ARRAY:
			glTexStorage2D(target, desc.texture.levels, internal_format, desc.texture.width, depth_or_layers);
			break;
		case GL_TEXTURE_CUBE_MAP:
			assert(depth_or_layers == 6);
			[[fallthrough]];
		case GL_TEXTURE_2D:
			glTexStorage2D(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height);
			break;
		case GL_TEXTURE_CUBE_MAP_ARRAY:
			assert((depth_or_layers % 6) == 0);
			depth_or_layers /= 6;
			[[fallthrough]];
		case GL_TEXTURE_2D_ARRAY:
		case GL_TEXTURE_3D:
			glTexStorage3D(target, desc.texture.levels, internal_format, desc.texture.width, desc.texture.height, depth_or_layers);
			break;
		}

		if (initial_data != nullptr)
		{
			for (uint32_t subresource = 0; subresource < static_cast<uint32_t>(desc.texture.depth_or_layers) * desc.texture.levels; ++subresource)
				upload_texture_region(initial_data[subresource], make_resource_handle(target, object), subresource, nullptr);
		}

		glBindTexture(target, prev_object);
	}

	*out = make_resource_handle(target, object);
	return true;
}
bool reshade::opengl::device_impl::create_resource_view(api::resource resource, api::resource_usage, const api::resource_view_desc &desc, api::resource_view *out)
{
	assert(resource.handle != 0);

	const bool is_srgb_format =
		desc.format != api::format_to_default_typed(desc.format, 0) &&
		desc.format == api::format_to_default_typed(desc.format, 1);

	const GLenum resource_target = resource.handle >> 40;
	if (resource_target == GL_RENDERBUFFER || resource_target == GL_FRAMEBUFFER_DEFAULT)
	{
		*out = make_resource_view_handle(resource_target, resource.handle & 0xFFFFFFFF, 0x1 | (is_srgb_format ? 0x2 : 0));
		return true;
	}

	GLenum target = GL_NONE;
	switch (desc.type)
	{
	case api::resource_view_type::buffer:
		target = GL_TEXTURE_BUFFER;
		break;
	case api::resource_view_type::texture_1d:
		target = GL_TEXTURE_1D;
		break;
	case api::resource_view_type::texture_1d_array:
		target = GL_TEXTURE_1D_ARRAY;
		break;
	case api::resource_view_type::texture_2d:
		target = GL_TEXTURE_2D;
		break;
	case api::resource_view_type::texture_2d_array:
		target = GL_TEXTURE_2D_ARRAY;
		break;
	case api::resource_view_type::texture_2d_multisample:
		target = GL_TEXTURE_2D_MULTISAMPLE;
		break;
	case api::resource_view_type::texture_2d_multisample_array:
		target = GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
		break;
	case api::resource_view_type::texture_3d:
		target = GL_TEXTURE_3D;
		break;
	case api::resource_view_type::texture_cube:
		target = GL_TEXTURE_CUBE_MAP;
		break;
	case api::resource_view_type::texture_cube_array:
		target = GL_TEXTURE_CUBE_MAP_ARRAY;
		break;
	default:
		assert(false);
		return false;
	}

	const GLenum internal_format = convert_format(desc.format);
	if (internal_format == GL_NONE)
		return false;

	if (target == resource_target &&
		desc.texture.first_level == 0 && desc.texture.first_layer == 0 &&
		static_cast<GLenum>(get_tex_level_param(target, resource.handle & 0xFFFFFFFF, 0, GL_TEXTURE_INTERNAL_FORMAT)) == internal_format)
	{
		assert(target != GL_TEXTURE_BUFFER);

		// No need to create a view, so use resource directly, but set a bit so to not destroy it twice via 'destroy_resource_view'
		*out = make_resource_view_handle(target, resource.handle & 0xFFFFFFFF, 0x1 | (is_srgb_format ? 0x2 : 0));
		return true;
	}
	else
	{
		GLuint object = 0;
		GLuint prev_object = 0;

		if (out != _current_event_handle)
		{
			glGenTextures(1, &object);
		}
		else
		{
			assert((out->handle >> 40) == target);
			object = out->handle & 0xFFFFFFFF;
		}

		if (target != GL_TEXTURE_BUFFER)
		{
			// Number of levels and layers are clamped to those of the original texture
			glTextureView(object, target, resource.handle & 0xFFFFFFFF, internal_format, desc.texture.first_level, desc.texture.levels, desc.texture.first_layer, desc.texture.layers);
		}
		else
		{
			glGetIntegerv(get_binding_for_target(target), reinterpret_cast<GLint *>(&prev_object));

			glBindTexture(target, object);

			if (desc.buffer.offset == 0 && desc.buffer.size == static_cast<uint64_t>(-1))
			{
				glTexBuffer(target, internal_format, resource.handle & 0xFFFFFFFF);
			}
			else
			{
				assert(desc.buffer.offset <= static_cast<uint64_t>(std::numeric_limits<GLintptr>::max()));
				assert(desc.buffer.size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()));
				glTexBufferRange(target, internal_format, resource.handle & 0xFFFFFFFF, static_cast<GLintptr>(desc.buffer.offset), static_cast<GLsizeiptr>(desc.buffer.size));
			}

			glBindTexture(target, prev_object);
		}

		*out = make_resource_view_handle(target, object, is_srgb_format ? 0x2 : 0);
		return true;
	}
}

static bool create_shader_module(GLenum type, const reshade::api::shader_desc &desc, GLuint &shader_object, bool existing_shader_object = false)
{
	if (!existing_shader_object)
		shader_object = 0;

	if (desc.code_size == 0)
		return false;

	if (!existing_shader_object)
		shader_object = glCreateShader(type);

	if (desc.format == reshade::api::shader_format::glsl)
	{
		assert(desc.entry_point == nullptr || strcmp(desc.entry_point, "main") == 0);
		assert(desc.num_spec_constants == 0);

		const auto source = static_cast<const GLchar *>(desc.code);
		const auto source_len = static_cast<GLint>(desc.code_size);
		glShaderSource(shader_object, 1, &source, &source_len);
		glCompileShader(shader_object);
	}
	else if (desc.format == reshade::api::shader_format::spirv)
	{
		assert(desc.code_size <= static_cast<size_t>(std::numeric_limits<GLsizei>::max()));

		glShaderBinary(1, &shader_object, GL_SPIR_V_BINARY, desc.code, static_cast<GLsizei>(desc.code_size));
		glSpecializeShader(shader_object, desc.entry_point, desc.num_spec_constants, desc.spec_constant_ids, desc.spec_constant_values);
	}

	GLint status = GL_FALSE;
	glGetShaderiv(shader_object, GL_COMPILE_STATUS, &status);
	if (GL_FALSE != status)
	{
		return true;
	}
	else
	{
		GLint log_size = 0;
		glGetShaderiv(shader_object, GL_INFO_LOG_LENGTH, &log_size);
		std::vector<char> log(log_size);
		glGetShaderInfoLog(shader_object, log_size, nullptr, log.data());

		LOG(ERROR) << "Failed to compile GLSL shader: " << log.data();

		glDeleteShader(shader_object);

		shader_object = 0;
		return false;
	}
}

bool reshade::opengl::device_impl::create_pipeline(const api::pipeline_desc &desc, api::pipeline *out)
{
	if (out != _current_event_handle)
		*out = { 0 };

	switch (desc.type)
	{
	default:
		*out = { 0 };
		return false;
	case api::pipeline_stage::all_graphics:
		return create_graphics_pipeline(desc, out);
	case api::pipeline_stage::vertex_shader:
		return create_shader_module(GL_VERTEX_SHADER, desc.graphics.vertex_shader, *reinterpret_cast<GLuint *>(out), out == _current_event_handle);
	case api::pipeline_stage::hull_shader:
		return create_shader_module(GL_TESS_CONTROL_SHADER, desc.graphics.hull_shader, *reinterpret_cast<GLuint *>(out), out == _current_event_handle);
	case api::pipeline_stage::domain_shader:
		return create_shader_module(GL_TESS_EVALUATION_SHADER, desc.graphics.domain_shader, *reinterpret_cast<GLuint *>(out), out == _current_event_handle);
	case api::pipeline_stage::geometry_shader:
		return create_shader_module(GL_GEOMETRY_SHADER, desc.graphics.geometry_shader, *reinterpret_cast<GLuint *>(out), out == _current_event_handle);
	case api::pipeline_stage::pixel_shader:
		return create_shader_module(GL_FRAGMENT_SHADER, desc.graphics.pixel_shader, *reinterpret_cast<GLuint *>(out), out == _current_event_handle);
	case api::pipeline_stage::compute_shader:
		if (out == _current_event_handle)
			return create_shader_module(GL_COMPUTE_SHADER, desc.compute.shader, *reinterpret_cast<GLuint *>(out), true);
		return create_compute_pipeline(desc, out);
	}
}
bool reshade::opengl::device_impl::create_compute_pipeline(const api::pipeline_desc &desc, api::pipeline *out)
{
	GLuint cs;
	const GLuint program = glCreateProgram();

	if (create_shader_module(GL_COMPUTE_SHADER, desc.compute.shader, cs))
		glAttachShader(program, cs);

	glLinkProgram(program);

	if (cs != 0)
		glDetachShader(program, cs);

	glDeleteShader(cs);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (GL_FALSE == status ||
		(desc.compute.shader.code_size != 0 && cs == 0))
	{
		GLint log_size = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_size);
		std::vector<char> log(log_size);
		glGetProgramInfoLog(program, log_size, nullptr, log.data());

		LOG(ERROR) << "Failed to link GLSL program:\n" << log.data();

		glDeleteProgram(program);

		*out = { 0 };
		return false;
	}

	const auto state = new pipeline_impl();
	state->program = program;

	*out = { reinterpret_cast<uintptr_t>(state) };
	return true;
}
bool reshade::opengl::device_impl::create_graphics_pipeline(const api::pipeline_desc &desc, api::pipeline *out)
{
	GLuint vs, hs, ds, gs, ps;
	const GLuint program = glCreateProgram();

	if (create_shader_module(GL_VERTEX_SHADER, desc.graphics.vertex_shader, vs))
		glAttachShader(program, vs);
	if (create_shader_module(GL_TESS_CONTROL_SHADER, desc.graphics.hull_shader, hs))
		glAttachShader(program, hs);
	if (create_shader_module(GL_TESS_EVALUATION_SHADER, desc.graphics.domain_shader, ds))
		glAttachShader(program, ds);
	if (create_shader_module(GL_GEOMETRY_SHADER, desc.graphics.geometry_shader, gs))
		glAttachShader(program, gs);
	if (create_shader_module(GL_FRAGMENT_SHADER, desc.graphics.pixel_shader, ps))
		glAttachShader(program, ps);

	glLinkProgram(program);

	if (vs != 0)
		glDetachShader(program, vs);
	if (hs != 0)
		glDetachShader(program, hs);
	if (ds != 0)
		glDetachShader(program, ds);
	if (gs != 0)
		glDetachShader(program, gs);
	if (ps != 0)
		glDetachShader(program, ps);

	glDeleteShader(vs);
	glDeleteShader(hs);
	glDeleteShader(ds);
	glDeleteShader(gs);
	glDeleteShader(ps);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	if (GL_FALSE == status ||
		(desc.graphics.vertex_shader.code_size != 0 && vs == 0) ||
		(desc.graphics.hull_shader.code_size != 0 && hs == 0) ||
		(desc.graphics.domain_shader.code_size != 0 && ds == 0) ||
		(desc.graphics.geometry_shader.code_size != 0 && gs == 0) ||
		(desc.graphics.pixel_shader.code_size != 0 && ps == 0))
	{
		GLint log_size = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_size);
		std::vector<char> log(log_size);
		glGetProgramInfoLog(program, log_size, nullptr, log.data());

		LOG(ERROR) << "Failed to link GLSL program: " << log.data();

		glDeleteProgram(program);

		*out = { 0 };
		return false;
	}

	const auto state = new pipeline_impl();
	state->program = program;

	{
		GLuint prev_vao = 0;
		glGenVertexArrays(1, &state->vao);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, reinterpret_cast<GLint *>(&prev_vao));

		glBindVertexArray(state->vao);

		for (uint32_t i = 0; i < 16 && desc.graphics.input_layout[i].format != api::format::unknown; ++i)
		{
			const auto &element = desc.graphics.input_layout[i];

			glEnableVertexAttribArray(element.location);

			GLint attrib_size = 0;
			GLboolean normalized = GL_FALSE;
			const GLenum attrib_format = convert_attrib_format(element.format, attrib_size, normalized);
#if 1
			glVertexAttribFormat(element.location, attrib_size, attrib_format, normalized, element.offset);
			glVertexAttribBinding(element.location, element.buffer_binding);
#else
			glVertexAttribPointer(element.location, attrib_size, attrib_format, normalized, element.stride, reinterpret_cast<const void *>(static_cast<uintptr_t>(element.offset)));
#endif
			glVertexBindingDivisor(element.buffer_binding, element.instance_step_rate);
		}

		glBindVertexArray(prev_vao);
	}

	state->sample_alpha_to_coverage = desc.graphics.blend_state.alpha_to_coverage_enable;
	state->blend_enable = desc.graphics.blend_state.blend_enable[0];
	state->logic_op_enable = desc.graphics.blend_state.logic_op_enable[0];
	state->blend_src = convert_blend_factor(desc.graphics.blend_state.src_color_blend_factor[0]);
	state->blend_dst = convert_blend_factor(desc.graphics.blend_state.dst_color_blend_factor[0]);
	state->blend_src_alpha = convert_blend_factor(desc.graphics.blend_state.src_alpha_blend_factor[0]);
	state->blend_dst_alpha = convert_blend_factor(desc.graphics.blend_state.dst_alpha_blend_factor[0]);
	state->blend_eq = convert_blend_op(desc.graphics.blend_state.color_blend_op[0]);
	state->blend_eq_alpha = convert_blend_op(desc.graphics.blend_state.alpha_blend_op[0]);
	state->logic_op = convert_logic_op(desc.graphics.blend_state.logic_op[0]);
	state->blend_constant[0] = ((desc.graphics.blend_state.blend_constant      ) & 0xFF) / 255.0f;
	state->blend_constant[1] = ((desc.graphics.blend_state.blend_constant >>  4) & 0xFF) / 255.0f;
	state->blend_constant[2] = ((desc.graphics.blend_state.blend_constant >>  8) & 0xFF) / 255.0f;
	state->blend_constant[3] = ((desc.graphics.blend_state.blend_constant >> 12) & 0xFF) / 255.0f;
	state->color_write_mask[0] = (desc.graphics.blend_state.render_target_write_mask[0] & (1 << 0)) != 0;
	state->color_write_mask[1] = (desc.graphics.blend_state.render_target_write_mask[0] & (1 << 1)) != 0;
	state->color_write_mask[2] = (desc.graphics.blend_state.render_target_write_mask[0] & (1 << 2)) != 0;
	state->color_write_mask[3] = (desc.graphics.blend_state.render_target_write_mask[0] & (1 << 3)) != 0;

	state->polygon_mode = convert_fill_mode(desc.graphics.rasterizer_state.fill_mode);
	state->cull_mode = convert_cull_mode(desc.graphics.rasterizer_state.cull_mode);
	state->front_face = desc.graphics.rasterizer_state.front_counter_clockwise ? GL_CCW : GL_CW;
	state->depth_clamp = !desc.graphics.rasterizer_state.depth_clip_enable;
	state->scissor_test = desc.graphics.rasterizer_state.scissor_enable;
	state->multisample_enable = desc.graphics.rasterizer_state.multisample_enable;
	state->line_smooth_enable = desc.graphics.rasterizer_state.antialiased_line_enable;

	// Polygon offset is not currently implemented
	assert(desc.graphics.rasterizer_state.depth_bias == 0 && desc.graphics.rasterizer_state.depth_bias_clamp == 0 && desc.graphics.rasterizer_state.slope_scaled_depth_bias == 0);

	state->depth_test = desc.graphics.depth_stencil_state.depth_enable;
	state->depth_mask = desc.graphics.depth_stencil_state.depth_write_mask;
	state->depth_func = convert_compare_op(desc.graphics.depth_stencil_state.depth_func);
	state->stencil_test = desc.graphics.depth_stencil_state.stencil_enable;
	state->stencil_read_mask = desc.graphics.depth_stencil_state.stencil_read_mask;
	state->stencil_write_mask = desc.graphics.depth_stencil_state.stencil_write_mask;
	state->stencil_reference_value = static_cast<GLint>(desc.graphics.depth_stencil_state.stencil_reference_value);
	state->front_stencil_op_fail = convert_stencil_op(desc.graphics.depth_stencil_state.front_stencil_fail_op);
	state->front_stencil_op_depth_fail = convert_stencil_op(desc.graphics.depth_stencil_state.front_stencil_depth_fail_op);
	state->front_stencil_op_pass = convert_stencil_op(desc.graphics.depth_stencil_state.front_stencil_pass_op);
	state->front_stencil_func = convert_compare_op(desc.graphics.depth_stencil_state.front_stencil_func);
	state->back_stencil_op_fail = convert_stencil_op(desc.graphics.depth_stencil_state.back_stencil_fail_op);
	state->back_stencil_op_depth_fail = convert_stencil_op(desc.graphics.depth_stencil_state.back_stencil_depth_fail_op);
	state->back_stencil_op_pass = convert_stencil_op(desc.graphics.depth_stencil_state.back_stencil_pass_op);
	state->back_stencil_func = convert_compare_op(desc.graphics.depth_stencil_state.back_stencil_func);

	state->sample_mask = desc.graphics.sample_mask;
	state->prim_mode = convert_primitive_topology(desc.graphics.topology);
	state->patch_vertices = state->prim_mode == GL_PATCHES ? static_cast<uint32_t>(desc.graphics.topology) - static_cast<uint32_t>(api::primitive_topology::patch_list_01_cp) : 0;

	*out = { reinterpret_cast<uintptr_t>(state) };
	return true;
}

bool reshade::opengl::device_impl::create_pipeline_layout(const api::pipeline_layout_desc &desc, api::pipeline_layout *out)
{
	if (desc.num_constant_ranges > 1)
	{
		*out = { 0 };
		return false;
	}

	const auto layout_impl = new pipeline_layout_impl();
	layout_impl->bindings.resize(desc.num_set_layouts + desc.num_constant_ranges);

	for (uint32_t i = 0; i < desc.num_set_layouts; ++i)
	{
		if (desc.set_layouts[i].handle == 0)
			continue;

		layout_impl->bindings[i] = reinterpret_cast<descriptor_set_layout_impl *>(desc.set_layouts[i].handle)->range.binding;
	}

	if (desc.num_constant_ranges == 1)
	{
		assert(desc.constant_ranges[0].offset == 0);
		layout_impl->bindings[desc.num_set_layouts] = desc.constant_ranges[0].dx_shader_register;
	}

	*out = { reinterpret_cast<uintptr_t>(layout_impl) };
	return true;
}
bool reshade::opengl::device_impl::create_descriptor_set_layout(const api::descriptor_set_layout_desc &desc, api::descriptor_set_layout *out)
{
	// Can only have descriptors of a single type in a descriptor set
	if (desc.num_ranges != 1)
	{
		*out = { 0 };
		return false;
	}

	const auto layout_impl = new descriptor_set_layout_impl();
	layout_impl->range = desc.ranges[0];

	*out = { reinterpret_cast<uintptr_t>(layout_impl) };
	return true;
}

bool reshade::opengl::device_impl::create_query_pool(api::query_type type, uint32_t size, api::query_pool *out)
{
	if (type == api::query_type::pipeline_statistics)
	{
		*out = { 0 };
		return false;
	}

	const auto result = new query_pool_impl();
	result->queries.resize(size);

	glGenQueries(static_cast<GLsizei>(size), result->queries.data());

	// Actually create and associate query objects with the names generated by 'glGenQueries' above
	for (GLuint i = 0; i < size; ++i)
	{
		if (type == api::query_type::timestamp)
		{
			glQueryCounter(result->queries[i], GL_TIMESTAMP);
		}
		else
		{
			const GLenum target = convert_query_type(type);
			glBeginQuery(target, result->queries[i]);
			glEndQuery(target);
		}
	}

	*out = { reinterpret_cast<uintptr_t>(result) };
	return true;
}
bool reshade::opengl::device_impl::create_render_pass(const api::render_pass_desc &desc, api::render_pass *out)
{
	if ((desc.render_targets[0].handle >> 40) == GL_FRAMEBUFFER_DEFAULT && (
		// Can only use both the color and depth-stencil attachments of the default framebuffer together, not bind them individually
		(desc.depth_stencil.handle == 0 || (desc.depth_stencil.handle >> 40) == GL_FRAMEBUFFER_DEFAULT)))
	{
		*out = make_render_pass_handle(0);
		return true;
	}

	GLuint prev_fbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint *>(&prev_fbo));

	GLuint fbo_object = 0;
	glGenFramebuffers(1, &fbo_object);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_object);

	bool has_srgb_attachment = false;
	uint32_t num_color_attachments = 0;

	for (GLuint i = 0; i < 8 && desc.render_targets[i].handle != 0; ++i, ++num_color_attachments)
	{
		switch (desc.render_targets[i].handle >> 40)
		{
		case GL_TEXTURE:
		case GL_TEXTURE_BUFFER:
		case GL_TEXTURE_1D:
		case GL_TEXTURE_1D_ARRAY:
		case GL_TEXTURE_2D:
		case GL_TEXTURE_2D_ARRAY:
		case GL_TEXTURE_2D_MULTISAMPLE:
		case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
		case GL_TEXTURE_RECTANGLE:
			glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, desc.render_targets[i].handle & 0xFFFFFFFF, 0);
			break;
		case GL_RENDERBUFFER:
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, desc.render_targets[i].handle & 0xFFFFFFFF);
			break;
		default:
			assert(false);
			glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
			glDeleteFramebuffers(1, &fbo_object);
			*out = { 0 };
			return false;
		}

		if (desc.render_targets[i].handle & 0x200000000)
			has_srgb_attachment = true;
	}

	if (desc.depth_stencil.handle != 0)
	{
		switch (desc.depth_stencil.handle >> 40)
		{
		case GL_TEXTURE:
		case GL_TEXTURE_BUFFER:
		case GL_TEXTURE_1D:
		case GL_TEXTURE_1D_ARRAY:
		case GL_TEXTURE_2D:
		case GL_TEXTURE_2D_ARRAY:
		case GL_TEXTURE_2D_MULTISAMPLE:
		case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
		case GL_TEXTURE_RECTANGLE:
			glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, desc.depth_stencil.handle & 0xFFFFFFFF, 0);
			break;
		case GL_RENDERBUFFER:
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, desc.depth_stencil.handle & 0xFFFFFFFF);
			break;
		default:
			assert(false);
			glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
			glDeleteFramebuffers(1, &fbo_object);
			*out = { 0 };
			return false;
		}
	}

	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);

	if (status == GL_FRAMEBUFFER_COMPLETE)
	{
		*out = make_render_pass_handle(fbo_object, num_color_attachments, has_srgb_attachment ? 0x2 : 0);
		return true;
	}
	else
	{
		glDeleteFramebuffers(1, &fbo_object);

		*out = { 0 };
		return false;
	}
}
bool reshade::opengl::device_impl::create_descriptor_sets(api::descriptor_set_layout layout, uint32_t count, api::descriptor_set *out)
{
	const auto layout_impl = reinterpret_cast<descriptor_set_layout_impl *>(layout.handle);

	for (uint32_t i = 0; i < count; ++i)
	{
		const auto set = new descriptor_set_impl();
		set->type = layout_impl->range.type;
		set->descriptors.resize(layout_impl->range.count * (set->type == api::descriptor_type::sampler_with_resource_view ? 2 : 1));

		out[i] = { reinterpret_cast<uintptr_t>(set) };
	}

	return true;
}

void reshade::opengl::device_impl::destroy_sampler(api::sampler handle)
{
	assert(handle.handle == 0 || (handle.handle >> 40) == GL_SAMPLER);

	const GLuint object = handle.handle & 0xFFFFFFFF;
	glDeleteSamplers(1, &object);
}
void reshade::opengl::device_impl::destroy_resource(api::resource handle)
{
	const GLuint object = handle.handle & 0xFFFFFFFF;
	switch (handle.handle >> 40)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		glDeleteBuffers(1, &object);
		break;
	case GL_TEXTURE:
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
		glDeleteTextures(1, &object);
		break;
	case GL_RENDERBUFFER:
		glDeleteRenderbuffers(1, &object);
		break;
	case GL_FRAMEBUFFER_DEFAULT:
		assert(false); // It is not allowed to destroy the default frame buffer
		break;
	default:
		assert(object == 0);
		break;
	}
}
void reshade::opengl::device_impl::destroy_resource_view(api::resource_view handle)
{
	if ((handle.handle & 0x100000000) == 0)
		destroy_resource({ handle.handle });
}

void reshade::opengl::device_impl::destroy_pipeline(api::pipeline_stage, api::pipeline handle)
{
	delete reinterpret_cast<pipeline_impl *>(handle.handle);
}
void reshade::opengl::device_impl::destroy_pipeline_layout(api::pipeline_layout handle)
{
	delete reinterpret_cast<pipeline_layout_impl *>(handle.handle);
}
void reshade::opengl::device_impl::destroy_descriptor_set_layout(api::descriptor_set_layout layout)
{
	delete reinterpret_cast<descriptor_set_layout_impl *>(layout.handle);
}

void reshade::opengl::device_impl::destroy_query_pool(api::query_pool handle)
{
	delete reinterpret_cast<query_pool_impl *>(handle.handle);
}
void reshade::opengl::device_impl::destroy_render_pass(api::render_pass handle)
{
	const GLuint object = handle.handle & 0xFFFFFFFF;
	glDeleteFramebuffers(1, &object);
}
void reshade::opengl::device_impl::destroy_descriptor_sets(api::descriptor_set_layout, uint32_t count, const api::descriptor_set *sets)
{
	for (uint32_t i = 0; i < count; ++i)
		delete reinterpret_cast<descriptor_set_impl *>(sets[i].handle);
}

void reshade::opengl::device_impl::update_descriptor_sets(uint32_t num_writes, const api::descriptor_set_write *writes, uint32_t num_copies, const api::descriptor_set_copy *copies)
{
	for (uint32_t i = 0; i < num_writes; ++i)
	{
		const auto set_impl = reinterpret_cast<descriptor_set_impl *>(writes[i].set.handle);

		const api::descriptor_set_write &info = writes[i];

		switch (info.type)
		{
		case api::descriptor_type::sampler:
			assert(info.descriptor.sampler.handle != 0);
			set_impl->descriptors[info.binding] = info.descriptor.sampler.handle;
			break;
		case api::descriptor_type::sampler_with_resource_view:
			assert(info.descriptor.sampler.handle != 0);
			set_impl->descriptors[info.binding * 2 + 0] = info.descriptor.sampler.handle;
			assert(info.descriptor.view.handle != 0);
			set_impl->descriptors[info.binding * 2 + 1] = info.descriptor.view.handle;
			break;
		case api::descriptor_type::shader_resource_view:
		case api::descriptor_type::unordered_access_view:
			assert(info.descriptor.view.handle != 0);
			set_impl->descriptors[info.binding] = info.descriptor.view.handle;
			break;
		case api::descriptor_type::constant_buffer:
			assert(info.descriptor.resource.handle != 0);
			assert(info.descriptor.offset == 0);
			set_impl->descriptors[info.binding] = info.descriptor.resource.handle;
			break;
		}
	}

	for (uint32_t i = 0; i < num_copies; ++i)
	{
		const auto src_set_impl = reinterpret_cast<descriptor_set_impl *>(copies[i].src_set.handle);
		const auto dst_set_impl = reinterpret_cast<descriptor_set_impl *>(copies[i].dst_set.handle);

		const api::descriptor_set_copy &info = copies[i];

		switch (info.type)
		{
		case api::descriptor_type::sampler:
		case api::descriptor_type::shader_resource_view:
		case api::descriptor_type::unordered_access_view:
		case api::descriptor_type::constant_buffer:
			for (uint32_t k = 0; k < info.count; ++k)
			{
				dst_set_impl->descriptors[info.dst_binding + k] = src_set_impl->descriptors[info.src_binding + k];
			}
			break;
		case api::descriptor_type::sampler_with_resource_view:
			for (uint32_t k = 0; k < info.count; ++k)
			{
				const uint32_t src_binding = (info.src_binding + k) * 2;
				const uint32_t dst_binding = (info.dst_binding + k) * 2;
				dst_set_impl->descriptors[dst_binding + 0] = src_set_impl->descriptors[src_binding + 0];
				dst_set_impl->descriptors[dst_binding + 1] = src_set_impl->descriptors[src_binding + 1];
			}
			break;
		}
	}
}

bool reshade::opengl::device_impl::map_resource(api::resource resource, uint32_t subresource, api::map_access access, void **data, uint32_t *row_pitch, uint32_t *slice_pitch)
{
	if (row_pitch != nullptr)
		*row_pitch = 0;
	if (slice_pitch != nullptr)
		*slice_pitch = 0;

	GLenum map_access = 0;
	switch (access)
	{
	case api::map_access::read_only:
		map_access = GL_MAP_READ_BIT;
		break;
	case api::map_access::write_only:
		map_access = GL_MAP_WRITE_BIT;
		break;
	case api::map_access::read_write:
		map_access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
		break;
	case api::map_access::write_discard:
		map_access = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
		break;
	}

	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	switch (target)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		assert(subresource == 0);
		if (gl3wProcs.gl.MapNamedBuffer != nullptr)
		{
			const GLuint length = get_buf_param(target, object, GL_BUFFER_SIZE);

			*data = glMapNamedBufferRange(object, 0, length, map_access);
		}
		else
		{
			const GLuint length = get_buf_param(target, object, GL_BUFFER_SIZE);

			GLint prev_object = 0;
			glGetIntegerv(get_binding_for_target(target), &prev_object);

			glBindBuffer(target, object);
			*data = glMapBufferRange(target, 0, length, map_access);
			glBindBuffer(target, prev_object);
		}
		break;
	default:
		assert(false);
		*data = nullptr;
		break;
	}

	return *data != nullptr;
}
void reshade::opengl::device_impl::unmap_resource(api::resource resource, uint32_t subresource)
{
	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	switch (target)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		assert(subresource == 0);
		if (gl3wProcs.gl.UnmapNamedBuffer != nullptr)
		{
			glUnmapNamedBuffer(object);
		}
		else
		{
			GLint prev_object = 0;
			glGetIntegerv(get_binding_for_target(target), &prev_object);

			glBindBuffer(target, object);
			glUnmapBuffer(target);
			glBindBuffer(target, prev_object);
		}
		break;
	default:
		assert(false);
		break;
	}
}

void reshade::opengl::device_impl::upload_buffer_region(const void *data, api::resource dst, uint64_t dst_offset, uint64_t size)
{
	assert(dst.handle != 0);
	assert(dst_offset <= static_cast<uint64_t>(std::numeric_limits<GLintptr>::max()) && size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()));

	const GLenum target = dst.handle >> 40;
	const GLuint object = dst.handle & 0xFFFFFFFF;

	// Get current state
	GLint previous_buf = 0;
	glGetIntegerv(get_binding_for_target(target), &previous_buf);

	// Bind and upload buffer data
	glBindBuffer(target, object);
	glBufferSubData(target, static_cast<GLintptr>(dst_offset), static_cast<GLsizeiptr>(size), data);

	// Restore previous state from application
	glBindBuffer(target, previous_buf);

}
void reshade::opengl::device_impl::upload_texture_region(const api::subresource_data &data, api::resource dst, uint32_t dst_subresource, const int32_t dst_box[6])
{
	assert(dst.handle != 0);
	const GLenum target = dst.handle >> 40;
	const GLuint object = dst.handle & 0xFFFFFFFF;

	// Get current state
	GLint previous_tex = 0;
	GLint previous_unpack = 0;
	GLint previous_unpack_lsb = GL_FALSE;
	GLint previous_unpack_swap = GL_FALSE;
	GLint previous_unpack_alignment = 0;
	GLint previous_unpack_row_length = 0;
	GLint previous_unpack_image_height = 0;
	GLint previous_unpack_skip_rows = 0;
	GLint previous_unpack_skip_pixels = 0;
	GLint previous_unpack_skip_images = 0;
	glGetIntegerv(get_binding_for_target(target), &previous_tex);
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previous_unpack);
	glGetIntegerv(GL_UNPACK_LSB_FIRST, &previous_unpack_lsb);
	glGetIntegerv(GL_UNPACK_SWAP_BYTES, &previous_unpack_swap);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_unpack_row_length);
	glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &previous_unpack_image_height);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &previous_unpack_skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &previous_unpack_skip_pixels);
	glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &previous_unpack_skip_images);

	// Unset any existing unpack buffer so pointer is not interpreted as an offset
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	// Clear pixel storage modes to defaults (texture uploads can break otherwise)
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

	// Bind and upload texture data
	glBindTexture(target, object);

	GLint levels = 1;
	glGetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
	const GLuint level = dst_subresource % levels;
	const GLuint layer = dst_subresource / levels;

	GLint format = GL_NONE; GLenum type;
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT, &format);

	GLint xoffset, yoffset, zoffset, width, height, depth;
	if (dst_box != nullptr)
	{
		xoffset = dst_box[0];
		yoffset = dst_box[1];
		zoffset = dst_box[2];
		width   = dst_box[3] - dst_box[0];
		height  = dst_box[4] - dst_box[1];
		depth   = dst_box[5] - dst_box[2];
	}
	else
	{
		xoffset = 0;
		yoffset = 0;
		zoffset = 0;
		glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH,  &width);
		glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(target, level, GL_TEXTURE_DEPTH,  &depth);
	}

	const auto row_size_packed = width * api::format_bpp(convert_format(format));
	const auto slice_size_packed = height * row_size_packed;
	const auto total_size = depth * slice_size_packed;

	format = convert_upload_format(format, type);

	std::vector<uint8_t> temp_pixels;
	const uint8_t *pixels = static_cast<const uint8_t *>(data.data);

	if ((row_size_packed != data.row_pitch && height == 1) ||
		(slice_size_packed != data.slice_pitch && depth == 1))
	{
		temp_pixels.resize(total_size);
		uint8_t *dst_pixels = temp_pixels.data();

		for (GLint z = 0; z < depth; ++z)
			for (GLint y = 0; y < height; ++y, dst_pixels += row_size_packed)
				std::memcpy(dst_pixels, pixels + z * data.slice_pitch + y * data.row_pitch, row_size_packed);

		pixels = temp_pixels.data();
	}

	switch (target)
	{
	case GL_TEXTURE_1D:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
		{
			glTexSubImage1D(target, level, xoffset, width, format, type, pixels);
		}
		else
		{
			glCompressedTexSubImage1D(target, level, xoffset, width, format, total_size, pixels);
		}
		break;
	case GL_TEXTURE_1D_ARRAY:
		yoffset += layer;
		[[fallthrough]];
	case GL_TEXTURE_2D:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
		{
			glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
		}
		else
		{
			glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, total_size, pixels);
		}
		break;
	case GL_TEXTURE_2D_ARRAY:
		zoffset += layer;
		[[fallthrough]];
	case GL_TEXTURE_3D:
		if (type != GL_COMPRESSED_TEXTURE_FORMATS)
		{
			glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
		}
		else
		{
			glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, total_size, pixels);
		}
		break;
	}

	// Restore previous state from application
	glBindTexture(target, previous_tex);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack);
	glPixelStorei(GL_UNPACK_LSB_FIRST, previous_unpack_lsb);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, previous_unpack_swap);
	glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_unpack_row_length);
	glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, previous_unpack_image_height);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, previous_unpack_skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, previous_unpack_skip_pixels);
	glPixelStorei(GL_UNPACK_SKIP_IMAGES, previous_unpack_skip_images);
}

bool reshade::opengl::device_impl::get_attachment(api::render_pass pass, api::attachment_type type, uint32_t index, api::resource_view *out) const
{
	assert(pass.handle != 0);
	const GLuint fbo_object = pass.handle & 0xFFFFFFFF;

	// Zero is valid too, in which case the default frame buffer is referenced, instead of a FBO
	if (fbo_object == 0)
	{
		if (type == api::attachment_type::color)
		{
			*out = make_resource_view_handle(GL_FRAMEBUFFER_DEFAULT, GL_BACK);
			return true;
		}
		if (_default_depth_format != GL_NONE)
		{
			*out = make_resource_view_handle(GL_FRAMEBUFFER_DEFAULT, GL_DEPTH_STENCIL_ATTACHMENT);
			return true;
		}
		else
		{
			*out = make_resource_view_handle(0, 0); // No default depth buffer exists
			return false;
		}
	}

	GLenum attachment = GL_NONE;
	switch (type)
	{
	case api::attachment_type::color:
		attachment = GL_COLOR_ATTACHMENT0 + index;
		if (index >= (pass.handle >> 40))
		{
			*out = make_resource_view_handle(0, 0);
			return false;
		}
		break;
	case api::attachment_type::depth:
		attachment = GL_DEPTH_ATTACHMENT;
		break;
	case api::attachment_type::stencil:
		attachment = GL_STENCIL_ATTACHMENT;
		break;
	default:
		if (type == (api::attachment_type::depth | api::attachment_type::stencil))
		{
			attachment = GL_DEPTH_STENCIL_ATTACHMENT;
			break;
		}
		else
		{
			*out = make_resource_view_handle(0, 0);
			return false;
		}
	}

	GLenum target = get_fbo_attachment_param(fbo_object, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE);
	if (target != GL_NONE)
	{
		const GLenum object = get_fbo_attachment_param(fbo_object, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME);
		if (target == GL_TEXTURE)
			target  = get_tex_param(target, object, GL_TEXTURE_TARGET);

		*out = make_resource_view_handle(target, object);
		return true;
	}
	else
	{
		*out = make_resource_view_handle(0, 0); // FBO does not have this attachment
		return false;
	}
}
uint32_t reshade::opengl::device_impl::get_attachment_count(api::render_pass pass, api::attachment_type type) const
{
	assert(pass.handle != 0);

	if (type == api::attachment_type::color)
		return pass.handle >> 40;
	else
		return 1;
}

void reshade::opengl::device_impl::get_resource_from_view(api::resource_view view, api::resource *out) const
{
	assert(view.handle != 0);

	// Remove extra bits from view
	*out = { view.handle & 0xFFFFFF00FFFFFFFF };
}

reshade::api::resource_desc reshade::opengl::device_impl::get_resource_desc(api::resource resource) const
{
	GLsizei width = 0, height = 1, depth = 1, levels = 1, samples = 1, buffer_size = 0; GLenum internal_format = GL_NONE;

	const GLenum target = resource.handle >> 40;
	const GLuint object = resource.handle & 0xFFFFFFFF;

	switch (target)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		buffer_size = get_buf_param(target, object, GL_BUFFER_SIZE);
		break;
	case GL_TEXTURE:
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
		width = get_tex_level_param(target, object, 0, GL_TEXTURE_WIDTH);
		height = get_tex_level_param(target, object, 0, GL_TEXTURE_HEIGHT);
		depth = get_tex_level_param(target, object, 0, GL_TEXTURE_DEPTH);
		internal_format = get_tex_level_param(target, object, 0, GL_TEXTURE_INTERNAL_FORMAT);
		if (get_tex_param(target, object, GL_TEXTURE_IMMUTABLE_FORMAT))
			levels = get_tex_param(target, object, GL_TEXTURE_IMMUTABLE_LEVELS);
		samples = get_tex_level_param(target, object, 0, GL_TEXTURE_SAMPLES);
		break;
	case GL_RENDERBUFFER:
		width = get_rbo_param(object, GL_RENDERBUFFER_WIDTH);
		height = get_rbo_param(object, GL_RENDERBUFFER_HEIGHT);
		internal_format = get_rbo_param(object, GL_RENDERBUFFER_INTERNAL_FORMAT);
		samples = get_rbo_param(object, GL_RENDERBUFFER_SAMPLES);
		break;
	case GL_FRAMEBUFFER_DEFAULT:
		width = _default_fbo_width;
		height = _default_fbo_height;
		internal_format = (object == GL_DEPTH_STENCIL_ATTACHMENT || object == GL_DEPTH_ATTACHMENT || object == GL_STENCIL_ATTACHMENT) ? _default_depth_format : _default_color_format;
		break;
	default:
		assert(false);
		break;
	}

	if (buffer_size != 0)
		return convert_resource_desc(target, buffer_size); // TODO: Memory heap
	else
		return convert_resource_desc(target, levels, samples, internal_format, width, height, depth);
}

bool reshade::opengl::device_impl::get_query_pool_results(api::query_pool pool, uint32_t first, uint32_t count, void *results, uint32_t stride)
{
	assert(pool.handle != 0);
	assert(stride >= sizeof(uint64_t));

	const auto impl = reinterpret_cast<query_pool_impl *>(pool.handle);

	for (uint32_t i = 0; i < count; ++i)
	{
		GLuint available = GL_FALSE;
		glGetQueryObjectuiv(impl->queries[i + first], GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			return false;

		glGetQueryObjectui64v(impl->queries[i + first], GL_QUERY_RESULT, reinterpret_cast<GLuint64 *>(static_cast<uint8_t *>(results) + i * stride));
	}

	return true;
}

void reshade::opengl::device_impl::wait_idle() const
{
	glFinish();
}

void reshade::opengl::device_impl::set_resource_name(api::resource resource, const char *name)
{
	GLenum id = resource.handle >> 40;
	switch (id)
	{
	case GL_BUFFER:
	case GL_ARRAY_BUFFER:
	case GL_ELEMENT_ARRAY_BUFFER:
	case GL_PIXEL_PACK_BUFFER:
	case GL_PIXEL_UNPACK_BUFFER:
	case GL_UNIFORM_BUFFER:
	case GL_TRANSFORM_FEEDBACK_BUFFER:
	case GL_COPY_READ_BUFFER:
	case GL_COPY_WRITE_BUFFER:
	case GL_DRAW_INDIRECT_BUFFER:
	case GL_SHADER_STORAGE_BUFFER:
	case GL_DISPATCH_INDIRECT_BUFFER:
	case GL_QUERY_BUFFER:
	case GL_ATOMIC_COUNTER_BUFFER:
		id = GL_BUFFER;
		break;
	case GL_TEXTURE:
	case GL_TEXTURE_BUFFER:
	case GL_TEXTURE_1D:
	case GL_TEXTURE_1D_ARRAY:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_2D_ARRAY:
	case GL_TEXTURE_2D_MULTISAMPLE:
	case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_ARRAY:
	case GL_TEXTURE_RECTANGLE:
		id = GL_TEXTURE;
		break;
	}

	glObjectLabel(id, resource.handle & 0xFFFFFFFF, -1, name);
}
