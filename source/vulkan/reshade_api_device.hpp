/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#pragma once

#include "addon_manager.hpp"
#pragma warning(push)
#pragma warning(disable: 4100 4127 4324 4703) // Disable a bunch of warnings thrown by VMA code
#include <vk_mem_alloc.h>
#pragma warning(pop)
#include <vk_layer_dispatch_table.h>
#include <mutex>
#include <cassert>
#include <unordered_map>

namespace reshade::vulkan
{
	struct resource_data
	{
		bool is_image() const { return type != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; }

		union
		{
			VkImage image;
			VkBuffer buffer;
		};
		union
		{
			VkStructureType type;
			VkImageCreateInfo image_create_info;
			VkBufferCreateInfo buffer_create_info;
		};

		VmaAllocation allocation;
		bool owned;
	};

	struct resource_view_data
	{
		bool is_image_view() const { return type != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO; }

		union
		{
			VkImageView image_view;
			VkBufferView buffer_view;
		};
		union
		{
			VkStructureType type;
			VkImageViewCreateInfo image_create_info;
			VkBufferViewCreateInfo buffer_create_info;
		};

		bool owned;
	};

	struct render_pass_data
	{
		struct attachment
		{
			VkImageLayout initial_layout;
			VkImageAspectFlags clear_flags;
			VkImageAspectFlags format_flags;
		};

		std::vector<attachment> attachments;
	};

	struct framebuffer_data
	{
		std::vector<api::resource_view> attachments;
		std::vector<VkImageAspectFlags> attachment_types;
	};

	class device_impl : public api::api_object_impl<VkDevice, api::device>
	{
		friend class command_list_impl;
		friend class command_queue_impl;

	public:
		device_impl(VkDevice device, VkPhysicalDevice physical_device, const VkLayerInstanceDispatchTable &instance_table, const VkLayerDispatchTable &device_table, const VkPhysicalDeviceFeatures &enabled_features);
		~device_impl();

		api::device_api get_api() const final { return api::device_api::vulkan; }

		bool check_capability(api::device_caps capability) const final;
		bool check_format_support(api::format format, api::resource_usage usage) const final;

		bool is_resource_handle_valid(api::resource handle) const final;
		bool is_resource_view_handle_valid(api::resource_view handle) const final;

		bool create_sampler(const api::sampler_desc &desc, api::sampler *out) final;
		bool create_resource(const api::resource_desc &desc, const api::subresource_data *initial_data, api::resource_usage initial_state, api::resource *out) final;
		bool create_resource_view(api::resource resource, api::resource_usage usage_type, const api::resource_view_desc &desc, api::resource_view *out) final;

		bool create_pipeline(const api::pipeline_desc &desc, api::pipeline *out) final;
		bool create_compute_pipeline(const api::pipeline_desc &desc, api::pipeline *out);
		bool create_graphics_pipeline(const api::pipeline_desc &desc, api::pipeline *out);

		bool create_pipeline_layout(const api::pipeline_layout_desc &desc, api::pipeline_layout *out) final;
		bool create_descriptor_set_layout(const api::descriptor_set_layout_desc &desc, api::descriptor_set_layout *out) final;

		bool create_query_pool(api::query_type type, uint32_t size, api::query_pool *out) final;
		bool create_render_pass(const api::render_pass_desc &desc, api::render_pass *out) final;
		bool create_descriptor_sets(api::descriptor_set_layout layout, uint32_t count, api::descriptor_set *out) final;

		void destroy_sampler(api::sampler handle) final;
		void destroy_resource(api::resource handle) final;
		void destroy_resource_view(api::resource_view handle) final;

		void destroy_pipeline(api::pipeline_stage type, api::pipeline handle) final;
		void destroy_pipeline_layout(api::pipeline_layout handle) final;
		void destroy_descriptor_set_layout(api::descriptor_set_layout handle) final;

		void destroy_query_pool(api::query_pool handle) final;
		void destroy_render_pass(api::render_pass handle) final;
		void destroy_descriptor_sets(api::descriptor_set_layout layout, uint32_t count, const api::descriptor_set *sets) final;

		bool get_attachment(api::render_pass pass, api::attachment_type type, uint32_t index, api::resource_view *out) const final;
		uint32_t get_attachment_count(api::render_pass pass, api::attachment_type type) const final;

		void get_resource_from_view(api::resource_view view, api::resource *out) const final;
		api::resource_desc get_resource_desc(api::resource resource) const final;

		bool map_resource(api::resource resource, uint32_t subresource, api::map_access access, void **data, uint32_t *row_pitch, uint32_t *slice_pitch) final;
		void unmap_resource(api::resource resource, uint32_t subresource) final;

		void upload_buffer_region(const void *data, api::resource dst, uint64_t dst_offset, uint64_t size) final;
		void upload_texture_region(const api::subresource_data &data, api::resource dst, uint32_t dst_subresource, const int32_t dst_box[6]) final;

		void update_descriptor_sets(uint32_t num_writes, const api::descriptor_set_write *writes, uint32_t num_copies, const api::descriptor_set_copy *copies) final;

		bool get_query_pool_results(api::query_pool pool, uint32_t first, uint32_t count, void *results, uint32_t stride) final;

		void wait_idle() const final;

		void set_resource_name(api::resource resource, const char *name) final;

		void advance_transient_descriptor_pool();

#if RESHADE_ADDON
		uint32_t get_subresource_index(VkImage image, const VkImageSubresourceLayers &layers, uint32_t layer = 0) const
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			return layers.mipLevel + (layers.baseArrayLayer + layer) * _resources.at((uint64_t)image).image_create_info.mipLevels;
		}

		api::resource_view get_default_view(VkImage image)
		{
			VkImageViewCreateInfo create_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			create_info.image = image;
			register_image_view((VkImageView)image, create_info); // Register fake image view for this image
			return { (uint64_t)image };
		}
#endif
		resource_data lookup_resource(api::resource resource) const
		{
			assert(resource.handle != 0);
			const std::lock_guard<std::mutex> lock(_mutex);
			return _resources.at(resource.handle);
		}
		resource_view_data lookup_resource_view(api::resource_view view) const
		{
			assert(view.handle != 0);
			const std::lock_guard<std::mutex> lock(_mutex);
			return _views.at(view.handle);
		}
		render_pass_data lookup_render_pass(VkRenderPass pass) const
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			return _render_pass_list.at(pass);
		}
		framebuffer_data lookup_framebuffer(VkFramebuffer fbo) const
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			return _framebuffer_list.at(fbo);
		}

		void register_image(VkImage image, const VkImageCreateInfo &create_info, VmaAllocation allocation = VK_NULL_HANDLE, bool owned = false)
		{
			resource_data data;
			data.image = image;
			data.image_create_info = create_info;
			data.allocation = allocation;
			data.owned = owned;

			const std::lock_guard<std::mutex> lock(_mutex);
			_resources.emplace((uint64_t)image, std::move(data));
		}
		void register_image_view(VkImageView image_view, const VkImageViewCreateInfo &create_info, bool owned = false)
		{
			resource_view_data data;
			data.image_view = image_view;
			data.image_create_info = create_info;
			data.owned = owned;

			const std::lock_guard<std::mutex> lock(_mutex);
			_views.emplace((uint64_t)image_view, std::move(data));
		}
		void register_buffer(VkBuffer buffer, const VkBufferCreateInfo &create_info, VmaAllocation allocation = VK_NULL_HANDLE, bool owned = false)
		{
			resource_data data;
			data.buffer = buffer;
			data.buffer_create_info = create_info;
			data.allocation = allocation;
			data.owned = owned;

			const std::lock_guard<std::mutex> lock(_mutex);
			_resources.emplace((uint64_t)buffer, std::move(data));
		}
		void register_buffer_view(VkBufferView buffer_view, const VkBufferViewCreateInfo &create_info, bool owned = false)
		{
			resource_view_data data;
			data.buffer_view = buffer_view;
			data.buffer_create_info = create_info;
			data.owned = owned;

			const std::lock_guard<std::mutex> lock(_mutex);
			_views.emplace((uint64_t)buffer_view, std::move(data));
		}
		void register_render_pass(VkRenderPass pass, render_pass_data &&data)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_render_pass_list.emplace(pass, std::move(data));
		}
		void register_framebuffer(VkFramebuffer fbo, framebuffer_data &&data)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_framebuffer_list.emplace(fbo, std::move(data));
		}

		void unregister_image(VkImage image)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_resources.erase((uint64_t)image);
		}
		void unregister_image_view(VkImageView image_view)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_views.erase((uint64_t)image_view);
		}
		void unregister_buffer(VkBuffer buffer)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_resources.erase((uint64_t)buffer);
		}
		void unregister_buffer_view(VkBufferView buffer_view)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_views.erase((uint64_t)buffer_view);
		}
		void unregister_render_pass(VkRenderPass pass)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_render_pass_list.erase(pass);
		}
		void unregister_framebuffer(VkFramebuffer fbo)
		{
			const std::lock_guard<std::mutex> lock(_mutex);
			_framebuffer_list.erase(fbo);
		}

		const VkPhysicalDevice _physical_device;
		const VkLayerDispatchTable _dispatch_table;
		const VkLayerInstanceDispatchTable _instance_dispatch_table;

		uint32_t _graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
		std::vector<command_queue_impl *> _queues;
		VkPhysicalDeviceFeatures _enabled_features = {};

#ifndef NDEBUG
		mutable bool _wait_for_idle_happened = false;
#endif

	private:
		bool create_shader_module(VkShaderStageFlagBits stage, const api::shader_desc &desc, VkPipelineShaderStageCreateInfo &stage_info, VkSpecializationInfo &spec_info, std::vector<VkSpecializationMapEntry> &spec_map);

		mutable std::mutex _mutex;

		VmaAllocator _alloc = nullptr;
		std::unordered_map<uint64_t, resource_data> _resources;
		std::unordered_map<uint64_t, resource_view_data> _views;

		std::unordered_map<VkRenderPass, render_pass_data> _render_pass_list;
		std::unordered_map<VkFramebuffer, framebuffer_data> _framebuffer_list;
		std::unordered_map<VkPipelineLayout, std::vector<VkDescriptorSetLayout>> _pipeline_layout_list;

		VkDescriptorPool _descriptor_pool = VK_NULL_HANDLE;
		VkDescriptorPool _transient_descriptor_pool[4] = {};
		uint32_t _transient_index = 0;
	};
}
