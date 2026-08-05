/*
 * Copyright 2023 Hans-Kristian Arntzen for Valve Corporation
 * Copyright 2026 Helios vGPU
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Helios addition: the interface between main.c and the debug-control facility
 * that used to live inside it.  See debug_control.c for why it was split out.
 */

#ifndef __VKD3D_D3D12CORE_DEBUG_CONTROL_H
#define __VKD3D_D3D12CORE_DEBUG_CONTROL_H

#include "vkd3d_core_interface.h"

/* The singleton handed out by d3d12core_D3D12GetInterface for
 * CLSID_VKD3DDebugControl / IID_IVKD3DDebugControlInterface.  It is the ONLY
 * way any of the state below is ever written. */
extern const struct IVKD3DDebugControlInterface vkd3d_debug_control_instance;

#endif /* __VKD3D_D3D12CORE_DEBUG_CONTROL_H */
