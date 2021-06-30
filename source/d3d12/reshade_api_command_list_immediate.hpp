/*
 * Copyright (C) 2021 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#pragma once

#include "reshade_api_command_list.hpp"

namespace reshade::d3d12
{
	class command_list_immediate_impl : public command_list_impl
	{
		static const UINT NUM_COMMAND_FRAMES = 4;

	public:
		command_list_immediate_impl(device_impl *device);
		~command_list_immediate_impl();

		bool flush(ID3D12CommandQueue *queue);
		bool flush_and_wait(ID3D12CommandQueue *queue);

		ID3D12GraphicsCommandList *const begin_commands() { _has_commands = true; return _orig; }

	private:
		UINT _cmd_index = 0;
		HANDLE _fence_event = nullptr;
		UINT64 _fence_value[NUM_COMMAND_FRAMES] = {};
		com_ptr<ID3D12Fence> _fence[NUM_COMMAND_FRAMES];
		com_ptr<ID3D12CommandAllocator> _cmd_alloc[NUM_COMMAND_FRAMES];
	};
}
