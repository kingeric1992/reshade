/*
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "dll_log.hpp"
#include "hook_manager.hpp"
#include "lockfree_table.hpp"
#include "vulkan_hooks.hpp"
#include "reshade_api_device.hpp"
#include "reshade_api_command_queue.hpp"
#include "reshade_api_swapchain.hpp"
#include "reshade_api_type_convert.hpp"

lockfree_table<void *, reshade::vulkan::device_impl *, 16> g_vulkan_devices;
static lockfree_table<VkQueue, reshade::vulkan::command_queue_impl *, 16> s_vulkan_queues;
extern lockfree_table<VkCommandBuffer, reshade::vulkan::command_list_impl *, 4096> g_vulkan_command_buffers;
extern lockfree_table<void *, VkLayerInstanceDispatchTable, 16> g_instance_dispatch;
extern lockfree_table<VkSurfaceKHR, HWND, 16> g_surface_windows;
static lockfree_table<VkSwapchainKHR, reshade::vulkan::swapchain_impl *, 16> s_vulkan_swapchains;

#define GET_DISPATCH_PTR(name, object) \
	GET_DISPATCH_PTR_FROM(name, g_vulkan_devices.at(dispatch_key_from_handle(object)))
#define GET_DISPATCH_PTR_FROM(name, data) \
	assert((data) != nullptr); \
	PFN_vk##name trampoline = (data)->_dispatch_table.name; \
	assert(trampoline != nullptr)
#define INIT_DISPATCH_PTR(name) \
	dispatch_table.name = reinterpret_cast<PFN_vk##name>(get_device_proc(device, "vk" #name))

static inline const char *vk_format_to_string(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_UNDEFINED:
		return "VK_FORMAT_UNDEFINED";
	case VK_FORMAT_R8G8B8A8_UNORM:
		return "VK_FORMAT_R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB:
		return "VK_FORMAT_R8G8B8A8_SRGB";
	case VK_FORMAT_B8G8R8A8_UNORM:
		return "VK_FORMAT_B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB:
		return "VK_FORMAT_B8G8R8A8_SRGB";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return "VK_FORMAT_R16G16B16A16_SFLOAT";
	default:
		return nullptr;
	}
}

VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	LOG(INFO) << "Redirecting " << "vkCreateDevice" << '(' << "physicalDevice = " << physicalDevice << ", pCreateInfo = " << pCreateInfo << ", pAllocator = " << pAllocator << ", pDevice = " << pDevice << ')' << " ...";

	assert(pCreateInfo != nullptr && pDevice != nullptr);

	// Look for layer link info if installed as a layer (provided by the Vulkan loader)
	VkLayerDeviceCreateInfo *const link_info = find_layer_info<VkLayerDeviceCreateInfo>(
		pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);

	// Get trampoline function pointers
	PFN_vkCreateDevice trampoline = nullptr;
	PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
	PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;

	if (link_info != nullptr)
	{
		assert(link_info->u.pLayerInfo != nullptr);
		assert(link_info->u.pLayerInfo->pfnNextGetDeviceProcAddr != nullptr);
		assert(link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr != nullptr);

		// Look up functions in layer info
		get_device_proc = link_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
		get_instance_proc = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
		trampoline = reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc(nullptr, "vkCreateDevice"));

		// Advance the link info for the next element on the chain
		link_info->u.pLayerInfo = link_info->u.pLayerInfo->pNext;
	}
#ifdef RESHADE_TEST_APPLICATION
	else
	{
		trampoline = reshade::hooks::call(vkCreateDevice);
		get_device_proc = reshade::hooks::call(vkGetDeviceProcAddr);
		get_instance_proc = reshade::hooks::call(vkGetInstanceProcAddr);
	}
#endif

	if (trampoline == nullptr) // Unable to resolve next 'vkCreateDevice' function in the call chain
		return VK_ERROR_INITIALIZATION_FAILED;

	LOG(INFO) << "> Dumping enabled device extensions:";
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
		LOG(INFO) << "  " << pCreateInfo->ppEnabledExtensionNames[i];

	auto enum_queue_families = g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)).GetPhysicalDeviceQueueFamilyProperties;
	assert(enum_queue_families != nullptr);
	auto enum_device_extensions = g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)).EnumerateDeviceExtensionProperties;
	assert(enum_device_extensions != nullptr);

	uint32_t num_queue_families = 0;
	enum_queue_families(physicalDevice, &num_queue_families, nullptr);
	std::vector<VkQueueFamilyProperties> queue_families(num_queue_families);
	enum_queue_families(physicalDevice, &num_queue_families, queue_families.data());

	uint32_t graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0, queue_family_index; i < pCreateInfo->queueCreateInfoCount; ++i)
	{
		queue_family_index = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
		assert(queue_family_index < num_queue_families);

		// Find the first queue family which supports graphics and has at least one queue
		if (pCreateInfo->pQueueCreateInfos[i].queueCount > 0 && (queue_families[queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			if (pCreateInfo->pQueueCreateInfos[i].pQueuePriorities[0] < 1.0f)
				LOG(WARN) << "Vulkan queue used for rendering has a low priority (" << pCreateInfo->pQueueCreateInfos[i].pQueuePriorities[0] << ").";

			graphics_queue_family_index = queue_family_index;
			break;
		}
	}

	VkPhysicalDeviceFeatures enabled_features = {};
	const VkPhysicalDeviceFeatures2 *features2 = find_in_structure_chain<VkPhysicalDeviceFeatures2>(
		pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
	if (features2 != nullptr) // The features from the structure chain take precedence
		enabled_features = features2->features;
	else if (pCreateInfo->pEnabledFeatures != nullptr)
		enabled_features = *pCreateInfo->pEnabledFeatures;

	std::vector<const char *> enabled_extensions;
	enabled_extensions.reserve(pCreateInfo->enabledExtensionCount);
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
		enabled_extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);

	// Check if the device is used for presenting
	if (std::find_if(enabled_extensions.begin(), enabled_extensions.end(),
		[](const char *name) { return strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }) == enabled_extensions.end())
	{
		LOG(WARN) << "Skipping device because it is not created with the \"" VK_KHR_SWAPCHAIN_EXTENSION_NAME "\" extension.";

		graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
	}
	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	else if (graphics_queue_family_index == std::numeric_limits<uint32_t>::max())
	{
		LOG(WARN) << "Skipping device because it is not created with a graphics queue.";
	}
	else
	{
		uint32_t num_extensions = 0;
		enum_device_extensions(physicalDevice, nullptr, &num_extensions, nullptr);
		std::vector<VkExtensionProperties> extensions(num_extensions);
		enum_device_extensions(physicalDevice, nullptr, &num_extensions, extensions.data());

		// Make sure the driver actually supports the requested extensions
		const auto add_extension = [&extensions, &enabled_extensions, &graphics_queue_family_index](const char *name, bool required) {
			if (const auto it = std::find_if(extensions.begin(), extensions.end(),
				[name](const auto &props) { return strncmp(props.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0; });
				it != extensions.end())
			{
				enabled_extensions.push_back(name);
				return true;
			}

			if (required)
			{
				LOG(ERROR) << "Required extension \"" << name << "\" is not supported on this device. Initialization failed.";

				// Reset queue family index to prevent ReShade initialization
				graphics_queue_family_index = std::numeric_limits<uint32_t>::max();
			}
			else
			{
				LOG(WARN)  << "Optional extension \"" << name << "\" is not supported on this device.";
			}

			return false;
		};

		// Enable features that ReShade requires
		enabled_features.shaderImageGatherExtended = true;
		enabled_features.shaderStorageImageWriteWithoutFormat = true;

		// Enable extensions that ReShade requires
		add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, false); // This is optional, see imgui code in 'swapchain_impl'
		add_extension(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, true);
		add_extension(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, true);
	}

	VkDeviceCreateInfo create_info = *pCreateInfo;
	create_info.enabledExtensionCount = uint32_t(enabled_extensions.size());
	create_info.ppEnabledExtensionNames = enabled_extensions.data();

	// Patch the enabled features
	if (features2 != nullptr)
		// This is evil, because overwriting application memory, but whatever (RenderDoc does this too)
		const_cast<VkPhysicalDeviceFeatures2 *>(features2)->features = enabled_features;
	else
		create_info.pEnabledFeatures = &enabled_features;

	// Continue calling down the chain
	const VkResult result = trampoline(physicalDevice, &create_info, pAllocator, pDevice);
	if (result < VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateDevice" << " failed with error code " << result << '.';
		return result;
	}

	VkDevice device = *pDevice;
	// Initialize the device dispatch table
	VkLayerDispatchTable dispatch_table = { get_device_proc };

	// ---- Core 1_0 commands
	INIT_DISPATCH_PTR(DestroyDevice);
	INIT_DISPATCH_PTR(GetDeviceQueue);
	INIT_DISPATCH_PTR(QueueSubmit);
	INIT_DISPATCH_PTR(QueueWaitIdle);
	INIT_DISPATCH_PTR(DeviceWaitIdle);
	INIT_DISPATCH_PTR(AllocateMemory);
	INIT_DISPATCH_PTR(FreeMemory);
	INIT_DISPATCH_PTR(MapMemory);
	INIT_DISPATCH_PTR(UnmapMemory);
	INIT_DISPATCH_PTR(FlushMappedMemoryRanges);
	INIT_DISPATCH_PTR(InvalidateMappedMemoryRanges);
	INIT_DISPATCH_PTR(BindBufferMemory);
	INIT_DISPATCH_PTR(BindImageMemory);
	INIT_DISPATCH_PTR(GetBufferMemoryRequirements);
	INIT_DISPATCH_PTR(GetImageMemoryRequirements);
	INIT_DISPATCH_PTR(CreateFence);
	INIT_DISPATCH_PTR(DestroyFence);
	INIT_DISPATCH_PTR(ResetFences);
	INIT_DISPATCH_PTR(GetFenceStatus);
	INIT_DISPATCH_PTR(WaitForFences);
	INIT_DISPATCH_PTR(CreateSemaphore);
	INIT_DISPATCH_PTR(DestroySemaphore);
	INIT_DISPATCH_PTR(CreateQueryPool);
	INIT_DISPATCH_PTR(DestroyQueryPool);
	INIT_DISPATCH_PTR(GetQueryPoolResults);
	INIT_DISPATCH_PTR(CreateBuffer);
	INIT_DISPATCH_PTR(DestroyBuffer);
	INIT_DISPATCH_PTR(CreateBufferView);
	INIT_DISPATCH_PTR(DestroyBufferView);
	INIT_DISPATCH_PTR(CreateImage);
	INIT_DISPATCH_PTR(DestroyImage);
	INIT_DISPATCH_PTR(GetImageSubresourceLayout);
	INIT_DISPATCH_PTR(CreateImageView);
	INIT_DISPATCH_PTR(DestroyImageView);
	INIT_DISPATCH_PTR(CreateShaderModule);
	INIT_DISPATCH_PTR(DestroyShaderModule);
	INIT_DISPATCH_PTR(CreateGraphicsPipelines);
	INIT_DISPATCH_PTR(CreateComputePipelines);
	INIT_DISPATCH_PTR(DestroyPipeline);
	INIT_DISPATCH_PTR(CreatePipelineLayout);
	INIT_DISPATCH_PTR(DestroyPipelineLayout);
	INIT_DISPATCH_PTR(CreateSampler);
	INIT_DISPATCH_PTR(DestroySampler);
	INIT_DISPATCH_PTR(CreateDescriptorSetLayout);
	INIT_DISPATCH_PTR(DestroyDescriptorSetLayout);
	INIT_DISPATCH_PTR(CreateDescriptorPool);
	INIT_DISPATCH_PTR(DestroyDescriptorPool);
	INIT_DISPATCH_PTR(ResetDescriptorPool);
	INIT_DISPATCH_PTR(AllocateDescriptorSets);
	INIT_DISPATCH_PTR(FreeDescriptorSets);
	INIT_DISPATCH_PTR(UpdateDescriptorSets);
	INIT_DISPATCH_PTR(CreateFramebuffer);
	INIT_DISPATCH_PTR(DestroyFramebuffer);
	INIT_DISPATCH_PTR(CreateRenderPass);
	INIT_DISPATCH_PTR(DestroyRenderPass);
	INIT_DISPATCH_PTR(CreateCommandPool);
	INIT_DISPATCH_PTR(DestroyCommandPool);
	INIT_DISPATCH_PTR(ResetCommandPool);
	INIT_DISPATCH_PTR(AllocateCommandBuffers);
	INIT_DISPATCH_PTR(FreeCommandBuffers);
	INIT_DISPATCH_PTR(BeginCommandBuffer);
	INIT_DISPATCH_PTR(EndCommandBuffer);
	INIT_DISPATCH_PTR(ResetCommandBuffer);
	INIT_DISPATCH_PTR(CmdBindPipeline);
	INIT_DISPATCH_PTR(CmdSetViewport);
	INIT_DISPATCH_PTR(CmdSetScissor);
	INIT_DISPATCH_PTR(CmdSetDepthBias);
	INIT_DISPATCH_PTR(CmdSetBlendConstants);
	INIT_DISPATCH_PTR(CmdSetStencilCompareMask);
	INIT_DISPATCH_PTR(CmdSetStencilWriteMask);
	INIT_DISPATCH_PTR(CmdSetStencilReference);
	INIT_DISPATCH_PTR(CmdBindDescriptorSets);
	INIT_DISPATCH_PTR(CmdBindIndexBuffer);
	INIT_DISPATCH_PTR(CmdBindVertexBuffers);
	INIT_DISPATCH_PTR(CmdDraw);
	INIT_DISPATCH_PTR(CmdDrawIndexed);
	INIT_DISPATCH_PTR(CmdDrawIndirect);
	INIT_DISPATCH_PTR(CmdDrawIndexedIndirect);
	INIT_DISPATCH_PTR(CmdDispatch);
	INIT_DISPATCH_PTR(CmdDispatchIndirect);
	INIT_DISPATCH_PTR(CmdCopyBuffer);
	INIT_DISPATCH_PTR(CmdCopyImage);
	INIT_DISPATCH_PTR(CmdBlitImage);
	INIT_DISPATCH_PTR(CmdCopyBufferToImage);
	INIT_DISPATCH_PTR(CmdCopyImageToBuffer);
	INIT_DISPATCH_PTR(CmdUpdateBuffer);
	INIT_DISPATCH_PTR(CmdClearColorImage);
	INIT_DISPATCH_PTR(CmdClearDepthStencilImage);
	INIT_DISPATCH_PTR(CmdClearAttachments);
	INIT_DISPATCH_PTR(CmdResolveImage);
	INIT_DISPATCH_PTR(CmdPipelineBarrier);
	INIT_DISPATCH_PTR(CmdBeginQuery);
	INIT_DISPATCH_PTR(CmdEndQuery);
	INIT_DISPATCH_PTR(CmdResetQueryPool);
	INIT_DISPATCH_PTR(CmdWriteTimestamp);
	INIT_DISPATCH_PTR(CmdCopyQueryPoolResults);
	INIT_DISPATCH_PTR(CmdPushConstants);
	INIT_DISPATCH_PTR(CmdBeginRenderPass);
	INIT_DISPATCH_PTR(CmdNextSubpass);
	INIT_DISPATCH_PTR(CmdEndRenderPass);
	INIT_DISPATCH_PTR(CmdExecuteCommands);
	// ---- Core 1_1 commands
	INIT_DISPATCH_PTR(BindBufferMemory2);
	INIT_DISPATCH_PTR(BindImageMemory2);
	INIT_DISPATCH_PTR(GetBufferMemoryRequirements2);
	INIT_DISPATCH_PTR(GetImageMemoryRequirements2);
	INIT_DISPATCH_PTR(GetDeviceQueue2);
	// ---- Core 1_2 commands
	INIT_DISPATCH_PTR(CreateRenderPass2);
	if (dispatch_table.CreateRenderPass2 == nullptr) // Try the KHR version if the core version does not exist
		dispatch_table.CreateRenderPass2  = reinterpret_cast<PFN_vkCreateRenderPass2KHR>(get_device_proc(device, "vkCreateRenderPass2KHR"));
	// ---- VK_KHR_swapchain extension commands
	INIT_DISPATCH_PTR(CreateSwapchainKHR);
	INIT_DISPATCH_PTR(DestroySwapchainKHR);
	INIT_DISPATCH_PTR(GetSwapchainImagesKHR);
	INIT_DISPATCH_PTR(QueuePresentKHR);
	// ---- VK_KHR_push_descriptor extension commands
	INIT_DISPATCH_PTR(CmdPushDescriptorSetKHR);
	// ---- VK_EXT_debug_utils extension commands
	INIT_DISPATCH_PTR(SetDebugUtilsObjectNameEXT);
	INIT_DISPATCH_PTR(QueueBeginDebugUtilsLabelEXT);
	INIT_DISPATCH_PTR(QueueEndDebugUtilsLabelEXT);
	INIT_DISPATCH_PTR(QueueInsertDebugUtilsLabelEXT);
	INIT_DISPATCH_PTR(CmdBeginDebugUtilsLabelEXT);
	INIT_DISPATCH_PTR(CmdEndDebugUtilsLabelEXT);
	INIT_DISPATCH_PTR(CmdInsertDebugUtilsLabelEXT);

	// Initialize per-device data
	const auto device_impl = new reshade::vulkan::device_impl(
		device,
		physicalDevice,
		g_instance_dispatch.at(dispatch_key_from_handle(physicalDevice)),
		dispatch_table,
		enabled_features);

	device_impl->_graphics_queue_family_index = graphics_queue_family_index;

	g_vulkan_devices.emplace(dispatch_key_from_handle(device), device_impl);

	// Initialize all queues associated with this device
	for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
	{
		const VkDeviceQueueCreateInfo &queue_create_info = pCreateInfo->pQueueCreateInfos[i];

		for (uint32_t queue_index = 0; queue_index < queue_create_info.queueCount; ++queue_index)
		{
			VkQueue queue = VK_NULL_HANDLE;
			dispatch_table.GetDeviceQueue(device, queue_create_info.queueFamilyIndex, queue_index, &queue);
			assert(VK_NULL_HANDLE != queue);

			const auto queue_impl = new reshade::vulkan::command_queue_impl(
				device_impl,
				queue_create_info.queueFamilyIndex,
				queue_families[queue_create_info.queueFamilyIndex],
				queue);

			s_vulkan_queues.emplace(queue, queue_impl);
		}
	}

#if RESHADE_VERBOSE_LOG
	LOG(INFO) << "Returning Vulkan device " << device << '.';
#endif
	return result;
}
void     VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	LOG(INFO) << "Redirecting " << "vkDestroyDevice" << '(' << "device = " << device << ", pAllocator = " << pAllocator << ')' << " ...";

	g_vulkan_command_buffers.clear(); // Reset all command buffer data

	// Remove from device dispatch table since this device is being destroyed
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.erase(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	// Destroy all queues associated with this device
	const std::vector<reshade::vulkan::command_queue_impl *> queues = device_impl->_queues;
	for (reshade::vulkan::command_queue_impl *queue_impl : queues)
	{
		s_vulkan_queues.erase(queue_impl->_orig);
		delete queue_impl; // This will remove the queue from the queue list of the device too (see 'command_queue_impl' destructor)
	}
	assert(device_impl->_queues.empty());

	// Get function pointer before data is destroyed next
	GET_DISPATCH_PTR_FROM(DestroyDevice, device_impl);

	// Finally destroy the device
	delete device_impl;

	trampoline(device, pAllocator);
}

VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	LOG(INFO) << "Redirecting " << "vkCreateSwapchainKHR" << '(' << "device = " << device << ", pCreateInfo = " << pCreateInfo << ", pAllocator = " << pAllocator << ", pSwapchain = " << pSwapchain << ')' << " ...";

	assert(pCreateInfo != nullptr && pSwapchain != nullptr);

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
	VkImageFormatListCreateInfoKHR format_list_info { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR };
	std::vector<VkFormat> format_list; std::vector<uint32_t> queue_family_list;

	// Only have to enable additional features if there is a graphics queue, since ReShade will not run otherwise
	if (device_impl->_graphics_queue_family_index != std::numeric_limits<uint32_t>::max())
	{
		// Add required usage flags to create info
		create_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		// Add required formats, so views with different formats can be created for the swap chain images
		format_list.push_back(reshade::vulkan::convert_format(
			reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 0)));
		format_list.push_back(reshade::vulkan::convert_format(
			reshade::api::format_to_default_typed(reshade::vulkan::convert_format(create_info.imageFormat), 1)));

		// Only have to make format mutable if they are actually different
		if (format_list[0] != format_list[1])
			create_info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;

		// Patch the format list in the create info of the application
		if (const VkImageFormatListCreateInfoKHR *format_list_info2 = find_in_structure_chain<VkImageFormatListCreateInfoKHR>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR); format_list_info2 != nullptr)
		{
			format_list.insert(format_list.end(),
				format_list_info2->pViewFormats, format_list_info2->pViewFormats + format_list_info2->viewFormatCount);

			// Remove duplicates from the list (since the new formats may have already been added by the application)
			std::sort(format_list.begin(), format_list.end());
			format_list.erase(std::unique(format_list.begin(), format_list.end()), format_list.end());

			// This is evil, because writing into the application memory, but eh =)
			const_cast<VkImageFormatListCreateInfoKHR *>(format_list_info2)->viewFormatCount = static_cast<uint32_t>(format_list.size());
			const_cast<VkImageFormatListCreateInfoKHR *>(format_list_info2)->pViewFormats = format_list.data();
		}
		else if (format_list[0] != format_list[1])
		{
			format_list_info.pNext = create_info.pNext;
			format_list_info.viewFormatCount = static_cast<uint32_t>(format_list.size());
			format_list_info.pViewFormats = format_list.data();

			create_info.pNext = &format_list_info;
		}

		// Add required queue family indices, so images can be used on the graphics queue
		if (create_info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
		{
			queue_family_list.reserve(create_info.queueFamilyIndexCount + 1);
			queue_family_list.push_back(device_impl->_graphics_queue_family_index);

			for (uint32_t i = 0; i < create_info.queueFamilyIndexCount; ++i)
				if (create_info.pQueueFamilyIndices[i] != device_impl->_graphics_queue_family_index)
					queue_family_list.push_back(create_info.pQueueFamilyIndices[i]);

			create_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_list.size());
			create_info.pQueueFamilyIndices = queue_family_list.data();
		}
	}

	LOG(INFO) << "> Dumping swap chain description:";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";
	LOG(INFO) << "  | Parameter                               | Value                                   |";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";
	LOG(INFO) << "  | flags                                   | " << std::setw(39) << std::hex << create_info.flags << std::dec << " |";
	LOG(INFO) << "  | surface                                 | " << std::setw(39) << create_info.surface << " |";
	LOG(INFO) << "  | minImageCount                           | " << std::setw(39) << create_info.minImageCount << " |";
	if (const char *format_string = vk_format_to_string(create_info.imageFormat); format_string != nullptr)
		LOG(INFO) << "  | imageFormat                             | " << std::setw(39) << format_string << " |";
	else
		LOG(INFO) << "  | imageFormat                             | " << std::setw(39) << create_info.imageFormat << " |";
	LOG(INFO) << "  | imageColorSpace                         | " << std::setw(39) << create_info.imageColorSpace << " |";
	LOG(INFO) << "  | imageExtent                             | " << std::setw(19) << create_info.imageExtent.width << ' ' << std::setw(19) << create_info.imageExtent.height << " |";
	LOG(INFO) << "  | imageArrayLayers                        | " << std::setw(39) << create_info.imageArrayLayers << " |";
	LOG(INFO) << "  | imageUsage                              | " << std::setw(39) << std::hex << create_info.imageUsage << std::dec << " |";
	LOG(INFO) << "  | imageSharingMode                        | " << std::setw(39) << create_info.imageSharingMode << " |";
	LOG(INFO) << "  | queueFamilyIndexCount                   | " << std::setw(39) << create_info.queueFamilyIndexCount << " |";
	LOG(INFO) << "  | preTransform                            | " << std::setw(39) << std::hex << create_info.preTransform << std::dec << " |";
	LOG(INFO) << "  | compositeAlpha                          | " << std::setw(39) << std::hex << create_info.compositeAlpha << std::dec << " |";
	LOG(INFO) << "  | presentMode                             | " << std::setw(39) << create_info.presentMode << " |";
	LOG(INFO) << "  | clipped                                 | " << std::setw(39) << (create_info.clipped ? "true" : "false") << " |";
	LOG(INFO) << "  | oldSwapchain                            | " << std::setw(39) << create_info.oldSwapchain << " |";
	LOG(INFO) << "  +-----------------------------------------+-----------------------------------------+";

#if RESHADE_ADDON
	reshade::api::resource_desc buffer_desc = {};
	buffer_desc.type = reshade::api::resource_type::texture_2d;
	buffer_desc.texture.width = create_info.imageExtent.width;
	buffer_desc.texture.height = create_info.imageExtent.height;
	assert(create_info.imageArrayLayers <= std::numeric_limits<uint16_t>::max());
	buffer_desc.texture.depth_or_layers = static_cast<uint16_t>(create_info.imageArrayLayers);
	buffer_desc.texture.levels = 1;
	buffer_desc.texture.format = reshade::vulkan::convert_format(create_info.imageFormat);
	buffer_desc.texture.samples = 1;
	buffer_desc.heap = reshade::api::memory_heap::gpu_only;
	reshade::vulkan::convert_image_usage_flags_to_usage(create_info.imageUsage, buffer_desc.usage);

	if (reshade::invoke_addon_event<reshade::addon_event::create_swapchain>(&buffer_desc))
	{
		create_info.imageFormat = reshade::vulkan::convert_format(buffer_desc.texture.format);
		create_info.imageExtent.width = buffer_desc.texture.width;
		create_info.imageExtent.height = buffer_desc.texture.height;
		create_info.imageArrayLayers = buffer_desc.texture.depth_or_layers;
		reshade::vulkan::convert_usage_to_image_usage_flags(buffer_desc.usage, create_info.imageUsage);
	}
#endif

	GET_DISPATCH_PTR_FROM(CreateSwapchainKHR, device_impl);
	const VkResult result = trampoline(device, &create_info, pAllocator, pSwapchain);
	if (result < VK_SUCCESS)
	{
		LOG(WARN) << "vkCreateSwapchainKHR" << " failed with error code " << result << '.';
		return result;
	}

	reshade::vulkan::command_queue_impl *queue_impl = nullptr;
	if (device_impl->_graphics_queue_family_index != std::numeric_limits<uint32_t>::max())
	{
		// Get the main graphics queue for command submission
		// There has to be at least one queue, or else this effect runtime would not have been created with this queue family index, so it is safe to get the first one here
		VkQueue graphics_queue = VK_NULL_HANDLE;
		device_impl->_dispatch_table.GetDeviceQueue(device, device_impl->_graphics_queue_family_index, 0, &graphics_queue);
		assert(VK_NULL_HANDLE != graphics_queue);

		queue_impl = s_vulkan_queues.at(graphics_queue);
	}

	if (queue_impl != nullptr)
	{
		// Remove old swap chain from the list so that a call to 'vkDestroySwapchainKHR' won't reset the effect runtime again
		reshade::vulkan::swapchain_impl *swapchain_impl = s_vulkan_swapchains.erase(create_info.oldSwapchain);
		if (swapchain_impl != nullptr)
		{
			assert(create_info.oldSwapchain != VK_NULL_HANDLE);

#if RESHADE_ADDON
			reshade::invoke_addon_event<reshade::addon_event::resize>(
				swapchain_impl, create_info.imageExtent.width, create_info.imageExtent.height);
#endif

			// Re-use the existing effect runtime if this swap chain was not created from scratch, but reset it before initializing again below
			swapchain_impl->on_reset();
		}
		else
		{
			swapchain_impl = new reshade::vulkan::swapchain_impl(device_impl, queue_impl);
		}

		// Look up window handle from surface
		const HWND hwnd = g_surface_windows.at(create_info.surface);

		if (!swapchain_impl->on_init(*pSwapchain, create_info, hwnd))
			LOG(ERROR) << "Failed to initialize Vulkan runtime environment on runtime " << swapchain_impl << '.';

		if (!s_vulkan_swapchains.emplace(*pSwapchain, swapchain_impl))
			delete swapchain_impl;
	}
	else
	{
		s_vulkan_swapchains.emplace(*pSwapchain, nullptr);
	}

#if RESHADE_VERBOSE_LOG
	LOG(INFO) << "Returning Vulkan swapchain " << *pSwapchain << '.';
#endif
	return result;
}
void     VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	LOG(INFO) << "Redirecting " << "vkDestroySwapchainKHR" << '(' << device << ", " << swapchain << ", " << pAllocator << ')' << " ...";

	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	assert(device_impl != nullptr);

	// Remove swap chain from global list
	delete s_vulkan_swapchains.erase(swapchain);

	GET_DISPATCH_PTR_FROM(DestroySwapchainKHR, device_impl);
	trampoline(device, swapchain, pAllocator);
}

VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	assert(pSubmits != nullptr);

#if RESHADE_ADDON
	if (reshade::vulkan::command_queue_impl *const queue_impl = s_vulkan_queues.at(queue); queue_impl != nullptr)
	{
		for (uint32_t i = 0; i < submitCount; ++i)
		{
			for (uint32_t k = 0; k < pSubmits[i].commandBufferCount; ++k)
			{
				assert(pSubmits[i].pCommandBuffers[k] != VK_NULL_HANDLE);

				if (reshade::vulkan::command_list_impl *const cmd_impl = g_vulkan_command_buffers.at(pSubmits[i].pCommandBuffers[k]); cmd_impl != nullptr)
				{
					reshade::invoke_addon_event<reshade::addon_event::execute_command_list>(queue_impl, cmd_impl);
				}
			}
		}

		queue_impl->flush_immediate_command_list();
	}
#endif

	// The loader uses the same dispatch table pointer for queues and devices, so can use queue to perform lookup here
	GET_DISPATCH_PTR(QueueSubmit, queue);
	return trampoline(queue, submitCount, pSubmits, fence);
}
VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	assert(pPresentInfo != nullptr);

	std::vector<VkSemaphore> wait_semaphores(
		pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);

	if (reshade::vulkan::command_queue_impl *const queue_impl = s_vulkan_queues.at(queue); queue_impl != nullptr)
	{
		for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
		{
			reshade::vulkan::swapchain_impl *const swapchain_impl = s_vulkan_swapchains.at(pPresentInfo->pSwapchains[i]);
			if (swapchain_impl != nullptr)
			{
#if RESHADE_ADDON
				reshade::invoke_addon_event<reshade::addon_event::present>(queue_impl, swapchain_impl);
#endif
				swapchain_impl->on_present(queue, pPresentInfo->pImageIndices[i], wait_semaphores);
			}
		}

		queue_impl->flush_immediate_command_list(wait_semaphores);

		static_cast<reshade::vulkan::device_impl *>(queue_impl->get_device())->advance_transient_descriptor_pool();
	}

	// Override wait semaphores based on the last queue submit from above
	VkPresentInfoKHR present_info = *pPresentInfo;
	present_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
	present_info.pWaitSemaphores = wait_semaphores.data();

	GET_DISPATCH_PTR(QueuePresentKHR, queue);
	return trampoline(queue, &present_info);
}

VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateBuffer, device_impl);

	assert(pCreateInfo != nullptr && pBuffer != nullptr);

#if RESHADE_ADDON
	const reshade::api::resource_desc desc = reshade::vulkan::convert_resource_desc(*pCreateInfo);
	assert(desc.heap == reshade::api::memory_heap::unknown);

	if (reshade::api::resource replacement = { 0 };
		reshade::invoke_addon_event<reshade::addon_event::create_resource>(
			device_impl, desc, nullptr, reshade::api::resource_usage::undefined, &replacement))
	{
		*pBuffer = (VkBuffer)replacement.handle;
		return VK_SUCCESS;
	}
#endif

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pBuffer);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		device_impl->register_buffer(*pBuffer, *pCreateInfo);

		reshade::invoke_addon_event<reshade::addon_event::init_resource>(
			device_impl, desc, nullptr, reshade::api::resource_usage::undefined, reshade::api::resource { (uint64_t)*pBuffer });
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateBuffer" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyBuffer, device_impl);

#if RESHADE_ADDON
	// Destroy resource via device implementation in case it was overwritten
	if (const reshade::vulkan::resource_data data = device_impl->lookup_resource({ (uint64_t)buffer }); data.owned)
	{
		device_impl->destroy_resource({ (uint64_t)buffer });
		return;
	}

	reshade::invoke_addon_event<reshade::addon_event::destroy_resource>(device_impl, reshade::api::resource { (uint64_t)buffer });

	device_impl->unregister_buffer(buffer);
#endif

	trampoline(device, buffer, pAllocator);
}

VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateBufferView, device_impl);

	assert(pCreateInfo != nullptr && pView != nullptr);

#if RESHADE_ADDON
	const reshade::api::resource_view_desc desc = reshade::vulkan::convert_resource_view_desc(*pCreateInfo);

	if (reshade::api::resource_view replacement = { 0 };
		reshade::invoke_addon_event<reshade::addon_event::create_resource_view>(
			device_impl, reshade::api::resource { (uint64_t)pCreateInfo->buffer }, reshade::api::resource_usage::undefined, desc, &replacement))
	{
		*pView = (VkBufferView)replacement.handle;
		return VK_SUCCESS;
	}
#endif

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pView);;
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		device_impl->register_buffer_view(*pView, *pCreateInfo);

		reshade::invoke_addon_event<reshade::addon_event::init_resource_view>(
			device_impl, reshade::api::resource { (uint64_t)pCreateInfo->buffer }, reshade::api::resource_usage::undefined, desc, reshade::api::resource_view { (uint64_t)*pView });
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateBufferView" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyBufferView, device_impl);

#if RESHADE_ADDON
	// Destroy resource view via device implementation in case it was overwritten
	if (const reshade::vulkan::resource_view_data data = device_impl->lookup_resource_view({ (uint64_t)bufferView }); data.owned)
	{
		device_impl->destroy_resource_view({ (uint64_t)bufferView });
		return;
	}

	reshade::invoke_addon_event<reshade::addon_event::destroy_resource_view>(device_impl, reshade::api::resource_view { (uint64_t)bufferView });

	device_impl->unregister_buffer_view(bufferView);
#endif

	trampoline(device, bufferView, pAllocator);
}

VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateImage, device_impl);

	assert(pCreateInfo != nullptr && pImage != nullptr);

#if RESHADE_ADDON
	const reshade::api::resource_desc desc = reshade::vulkan::convert_resource_desc(*pCreateInfo);
	assert(desc.heap == reshade::api::memory_heap::unknown);

	if (reshade::api::resource replacement = { 0 };
		reshade::invoke_addon_event<reshade::addon_event::create_resource>(
			device_impl, desc, nullptr, pCreateInfo->initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ? reshade::api::resource_usage::cpu_access : reshade::api::resource_usage::undefined, &replacement))
	{
		*pImage = (VkImage)replacement.handle;
		return VK_SUCCESS;
	}
#endif

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pImage);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		device_impl->register_image(*pImage, *pCreateInfo);

		reshade::invoke_addon_event<reshade::addon_event::init_resource>(
			device_impl, desc, nullptr, pCreateInfo->initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ? reshade::api::resource_usage::cpu_access : reshade::api::resource_usage::undefined, reshade::api::resource { (uint64_t)*pImage });
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateImage" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyImage, device_impl);

#if RESHADE_ADDON
	// Destroy resource via device implementation in case it was overwritten
	if (const reshade::vulkan::resource_data data = device_impl->lookup_resource({ (uint64_t)image }); data.owned)
	{
		device_impl->destroy_resource({ (uint64_t)image });
		return;
	}

	reshade::invoke_addon_event<reshade::addon_event::destroy_resource>(device_impl, reshade::api::resource { (uint64_t)image });

	device_impl->unregister_image(image);
#endif

	trampoline(device, image, pAllocator);
}

VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateImageView, device_impl);

	assert(pCreateInfo != nullptr && pView != nullptr);

#if RESHADE_ADDON
	const reshade::api::resource_view_desc desc = reshade::vulkan::convert_resource_view_desc(*pCreateInfo);

	if (reshade::api::resource_view replacement = { 0 };
		reshade::invoke_addon_event<reshade::addon_event::create_resource_view>(
			device_impl, reshade::api::resource { (uint64_t)pCreateInfo->image }, reshade::api::resource_usage::undefined, desc, &replacement))
	{
		*pView = (VkImageView)replacement.handle;
		return VK_SUCCESS;
	}
#endif

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pView);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		device_impl->register_image_view(*pView, *pCreateInfo);

		reshade::invoke_addon_event<reshade::addon_event::init_resource_view>(
			device_impl, reshade::api::resource { (uint64_t)pCreateInfo->image }, reshade::api::resource_usage::undefined, desc, reshade::api::resource_view { (uint64_t)*pView });
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateImageView" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyImageView, device_impl);

#if RESHADE_ADDON
	// Destroy resource view via device implementation in case it was overwritten
	if (const reshade::vulkan::resource_view_data data = device_impl->lookup_resource_view({ (uint64_t)imageView }); data.owned)
	{
		device_impl->destroy_resource_view({ (uint64_t)imageView });
		return;
	}

	reshade::invoke_addon_event<reshade::addon_event::destroy_resource_view>(device_impl, reshade::api::resource_view { (uint64_t)imageView });

	device_impl->unregister_image_view(imageView);
#endif

	trampoline(device, imageView, pAllocator);
}

VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateShaderModule, device_impl);

	assert(pCreateInfo != nullptr && pShaderModule != nullptr);

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pShaderModule);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateShaderModule" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyShaderModule, device_impl);

#if RESHADE_ADDON
#endif

	trampoline(device, shaderModule, pAllocator);
}

VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateGraphicsPipelines, device_impl);

#if RESHADE_ADDON
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < createInfoCount; ++i)
	{
		const reshade::api::pipeline_desc desc = reshade::vulkan::convert_pipeline_desc(pCreateInfos[i]);

		if (reshade::api::pipeline replacement = { 0 };
			reshade::invoke_addon_event<reshade::addon_event::create_pipeline>(device_impl, desc, &replacement))
		{
			pPipelines[i] = (VkPipeline)replacement.handle;
			continue;
		}

		result = trampoline(device, pipelineCache, 1, &pCreateInfos[i], pAllocator, &pPipelines[i]);
		if (result >= VK_SUCCESS)
		{
			reshade::invoke_addon_event<reshade::addon_event::init_pipeline>(device_impl, desc, reshade::api::pipeline { (uint64_t)pPipelines[i] });
		}
		else
		{
#if RESHADE_VERBOSE_LOG
			LOG(WARN) << "vkCreateGraphicsPipelines" << " failed with error code " << result << '.';
#endif

			for (uint32_t k = 0; k < i; ++k)
				vkDestroyPipeline(device, pPipelines[k], pAllocator);
			for (uint32_t k = 0; k < createInfoCount; ++k)
				pPipelines[k] = VK_NULL_HANDLE;
			break;
		}
	}

	return result;
#else
	return trampoline(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
#endif
}
VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateComputePipelines, device_impl);

#if RESHADE_ADDON
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < createInfoCount; ++i)
	{
		const reshade::api::pipeline_desc desc = reshade::vulkan::convert_pipeline_desc(pCreateInfos[i]);

		if (reshade::api::pipeline replacement = { 0 };
			reshade::invoke_addon_event<reshade::addon_event::create_pipeline>(device_impl, desc, &replacement))
		{
			pPipelines[i] = (VkPipeline)replacement.handle;
			continue;
		}

		result = trampoline(device, pipelineCache, 1, &pCreateInfos[i], pAllocator, &pPipelines[i]);
		if (result >= VK_SUCCESS)
		{
			reshade::invoke_addon_event<reshade::addon_event::init_pipeline>(device_impl, desc, reshade::api::pipeline { (uint64_t)pPipelines[i] });
		}
		else
		{
#if RESHADE_VERBOSE_LOG
			LOG(WARN) << "vkCreateComputePipelines" << " failed with error code " << result << '.';
#endif

			for (uint32_t k = 0; k < i; ++k)
				vkDestroyPipeline(device, pPipelines[k], pAllocator);
			for (uint32_t k = 0; k < createInfoCount; ++k)
				pPipelines[k] = VK_NULL_HANDLE;
			break;
		}
	}

	return result;
#else
	return trampoline(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
#endif
}
void     VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyPipeline, device_impl);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::destroy_pipeline>(device_impl, reshade::api::pipeline { (uint64_t)pipeline });
#endif

	trampoline(device, pipeline, pAllocator);
}

VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateSampler, device_impl);

	assert(pCreateInfo != nullptr && pSampler != nullptr);

#if RESHADE_ADDON
	const reshade::api::sampler_desc desc = reshade::vulkan::convert_sampler_desc(*pCreateInfo);

	if (reshade::api::sampler replacement = { 0 };
		reshade::invoke_addon_event<reshade::addon_event::create_sampler>(device_impl, desc, &replacement))
	{
		*pSampler = (VkSampler)replacement.handle;
		return VK_SUCCESS;
	}
#endif

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pSampler);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		reshade::invoke_addon_event<reshade::addon_event::init_sampler>(
			device_impl, desc, reshade::api::sampler { (uint64_t)*pSampler });
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateSampler" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroySampler, device_impl);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::destroy_sampler>(device_impl, reshade::api::sampler { (uint64_t)sampler });
#endif

	trampoline(device, sampler, pAllocator);
}

void     VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(UpdateDescriptorSets, device_impl);

#if RESHADE_ADDON
	std::vector<reshade::api::descriptor_set_copy> copies(descriptorCopyCount);
	std::vector<reshade::api::descriptor_set_write> writes(descriptorWriteCount);

	for (uint32_t i = 0; i < descriptorWriteCount; ++i)
	{
		const VkWriteDescriptorSet &write = pDescriptorWrites[i];

		writes[i].set = { (uint64_t)write.dstSet };
		writes[i].binding = write.dstBinding;
		writes[i].array_offset = write.dstArrayElement;
		// TODO: writes[i].count = write.descriptorCount;
		writes[i].type = static_cast<reshade::api::descriptor_type>(write.descriptorType);

		switch (write.descriptorType)
		{
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			writes[i].descriptor.sampler = { (uint64_t)write.pImageInfo[0].sampler };
			break;
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			writes[i].descriptor.view = { (uint64_t)write.pImageInfo[0].imageView };
			writes[i].descriptor.sampler = { (uint64_t)write.pImageInfo[0].sampler };
			break;
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			writes[i].descriptor.view = { (uint64_t)write.pImageInfo[0].imageView };
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			writes[i].descriptor.resource = { (uint64_t)write.pBufferInfo[0].buffer };
			break;
		}
	}
	for (uint32_t i = 0; i < descriptorCopyCount; ++i)
	{
		const VkCopyDescriptorSet &copy = pDescriptorCopies[i];

		copies[i].src_set = { (uint64_t)copy.srcSet };
		copies[i].src_binding = copy.srcBinding;
		copies[i].src_array_offset = copy.srcArrayElement;
		copies[i].dst_set = { (uint64_t)copy.dstSet };
		copies[i].dst_binding = copy.dstBinding;
		copies[i].dst_array_offset = copy.dstArrayElement;
		copies[i].count = copy.descriptorCount;
		// TODO: copies[i].type = reshade::api::descriptor_type::constant_buffer;
	}

	if (reshade::invoke_addon_event<reshade::addon_event::update_descriptor_sets>(device_impl, descriptorWriteCount, writes.data(), descriptorCopyCount, copies.data()))
		return;
#endif

	trampoline(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateFramebuffer, device_impl);

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pFramebuffer);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		const auto render_pass_info = device_impl->lookup_render_pass(pCreateInfo->renderPass);

		// Keep track of the frame buffer attachments
		reshade::vulkan::framebuffer_data data;
		data.attachments.resize(pCreateInfo->attachmentCount);
		data.attachment_types.resize(pCreateInfo->attachmentCount);
		for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
		{
			data.attachments[i] = { (uint64_t)pCreateInfo->pAttachments[i] };
			data.attachment_types[i] = render_pass_info.attachments[i].format_flags;
		}

		device_impl->register_framebuffer(*pFramebuffer, std::move(data));
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateFramebuffer" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyFramebuffer, device_impl);

#if RESHADE_ADDON
	device_impl->unregister_framebuffer(framebuffer);
#endif

	trampoline(device, framebuffer, pAllocator);
}

VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateRenderPass, device_impl);

	assert(pCreateInfo != nullptr && pRenderPass != nullptr);

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pRenderPass);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		reshade::vulkan::render_pass_data renderpass_data;
		renderpass_data.attachments.reserve(pCreateInfo->attachmentCount);

		for (uint32_t attachment = 0; attachment < pCreateInfo->attachmentCount; ++attachment)
		{
			extern VkImageAspectFlags aspect_flags_from_format(VkFormat format);
			VkImageAspectFlags clear_flags = aspect_flags_from_format(pCreateInfo->pAttachments[attachment].format);
			const VkImageAspectFlags format_flags = clear_flags;

			if (pCreateInfo->pAttachments[attachment].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
				clear_flags &= ~(VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT);
			if (pCreateInfo->pAttachments[attachment].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
				clear_flags &= ~(VK_IMAGE_ASPECT_STENCIL_BIT);

			renderpass_data.attachments.push_back({ pCreateInfo->pAttachments[attachment].initialLayout, clear_flags, format_flags });
		}

		device_impl->register_render_pass(*pRenderPass, std::move(renderpass_data));
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateRenderPass" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
VkResult VKAPI_CALL vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(CreateRenderPass2, device_impl);

	assert(pCreateInfo != nullptr && pRenderPass != nullptr);

	const VkResult result = trampoline(device, pCreateInfo, pAllocator, pRenderPass);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		reshade::vulkan::render_pass_data renderpass_data;
		renderpass_data.attachments.reserve(pCreateInfo->attachmentCount);

		for (uint32_t attachment = 0; attachment < pCreateInfo->attachmentCount; ++attachment)
		{
			extern VkImageAspectFlags aspect_flags_from_format(VkFormat format);
			VkImageAspectFlags clear_flags = aspect_flags_from_format(pCreateInfo->pAttachments[attachment].format);
			const VkImageAspectFlags format_flags = clear_flags;

			if (pCreateInfo->pAttachments[attachment].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
				clear_flags &= ~(VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT);
			if (pCreateInfo->pAttachments[attachment].stencilLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
				clear_flags &= ~(VK_IMAGE_ASPECT_STENCIL_BIT);

			renderpass_data.attachments.push_back({ pCreateInfo->pAttachments[attachment].initialLayout, clear_flags, format_flags });
		}

		device_impl->register_render_pass(*pRenderPass, std::move(renderpass_data));
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkCreateRenderPass2" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(DestroyRenderPass, device_impl);

#if RESHADE_ADDON
	device_impl->unregister_render_pass(renderPass);
#endif

	trampoline(device, renderPass, pAllocator);
}

VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers)
{
	reshade::vulkan::device_impl *const device_impl = g_vulkan_devices.at(dispatch_key_from_handle(device));
	GET_DISPATCH_PTR_FROM(AllocateCommandBuffers, device_impl);

	const VkResult result = trampoline(device, pAllocateInfo, pCommandBuffers);
	if (result >= VK_SUCCESS)
	{
#if RESHADE_ADDON
		for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
		{
			const auto cmd_impl = new reshade::vulkan::command_list_impl(device_impl, pCommandBuffers[i]);
			if (!g_vulkan_command_buffers.emplace(pCommandBuffers[i], cmd_impl))
				delete cmd_impl;
		}
#endif
	}
	else
	{
#if RESHADE_VERBOSE_LOG
		LOG(WARN) << "vkAllocateCommandBuffers" << " failed with error code " << result << '.';
#endif
	}

	return result;
}
void     VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
#if RESHADE_ADDON
	for (uint32_t i = 0; i < commandBufferCount; ++i)
	{
		delete g_vulkan_command_buffers.erase(pCommandBuffers[i]);
	}
#endif

	GET_DISPATCH_PTR(FreeCommandBuffers, device);
	trampoline(device, commandPool, commandBufferCount, pCommandBuffers);
}
