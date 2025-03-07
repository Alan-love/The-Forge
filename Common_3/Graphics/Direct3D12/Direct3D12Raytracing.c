/*
 * Copyright (c) 2017-2025 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "../GraphicsConfig.h"

#ifdef DIRECT3D12

// Socket is used in microprofile this header need to be included before d3d12 headers
#include <WinSock2.h>

// OS
#include "../../Utilities/ThirdParty/OpenSource/Nothings/stb_ds.h"

#include "../../Utilities/Interfaces/ILog.h"

// Renderer
#include "../../Resources/ResourceLoader/ThirdParty/OpenSource/tinyimageformat/tinyimageformat_apis.h"

#include "../Interfaces/IGraphics.h"
#include "../Interfaces/IRay.h"

#include "Direct3D12Hooks.h"

#include "../../Utilities/Interfaces/IMemory.h"

// check if WindowsSDK is used which supports raytracing
#ifdef D3D12_RAYTRACING_AVAILABLE

void addBuffer(Renderer* pRenderer, const BufferDesc* pDesc, Buffer** pp_buffer);
void removeBuffer(Renderer* pRenderer, Buffer* pBuffer);

// Enable experimental features and return if they are supported.
// To test them being supported we need to check both their enablement as well as device creation afterwards.
inline bool EnableD3D12ExperimentalFeatures(UUID* experimentalFeatures, uint32_t featureCount)
{
    ID3D12Device* testDevice = NULL;
    bool          ret = SUCCEEDED(D3D12EnableExperimentalFeatures(featureCount, experimentalFeatures, NULL, NULL)) &&
               SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_ARGS(ID3D12Device, &testDevice)));
    if (ret)
        COM_CALL(Release, testDevice);

    return ret;
}

// Enable experimental features required for compute-based raytracing fallback.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableComputeRaytracingFallback()
{
    UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };
    return EnableD3D12ExperimentalFeatures(experimentalFeatures, 1);
}

/************************************************************************/
// Utility Functions Declarations
/************************************************************************/
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS util_to_dx_acceleration_structure_build_flags(AccelerationStructureBuildFlags flags);
D3D12_RAYTRACING_GEOMETRY_FLAGS                     util_to_dx_geometry_flags(AccelerationStructureGeometryFlags flags);
D3D12_RAYTRACING_INSTANCE_FLAGS                     util_to_dx_instance_flags(AccelerationStructureInstanceFlags flags);
/************************************************************************/
// Forge Raytracing Implementation using DXR
/************************************************************************/
typedef struct Raytracing
{
    Renderer*             pRenderer;
    ID3D12Device5*        prDevice;
    D3D12_RAYTRACING_TIER mTier;
} Raytracing;

typedef struct AccelerationStructure
{
    Buffer* pASBuffer;
    Buffer* pScratchBuffer;
    union
    {
        struct
        {
            D3D12_RAYTRACING_GEOMETRY_DESC* pGeometryDescs;
        };
        struct
        {
            Buffer* pInstanceDescBuffer;
        };
    };
    uint32_t                                            mDescCount;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS mFlags;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE        mType;
} AccelerationStructure;

EXTERN_C const GUID IID_ID3D12Device5_Copy;

bool initRaytracing(Renderer* pRenderer, Raytracing** ppRaytracing)
{
    ASSERT(pRenderer);
    ASSERT(ppRaytracing);

    if (!pRenderer->pGpu->mRaytracingSupported)
    {
        return false;
    }

    Raytracing* pRaytracing = (Raytracing*)tf_calloc(1, sizeof(*pRaytracing));
    ASSERT(pRaytracing);

    pRaytracing->pRenderer = pRenderer;
    COM_CALL(QueryInterface, pRenderer->mDx.pDevice, IID_REF(ID3D12Device5_Copy), &pRaytracing->prDevice);

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = { 0 };
    HRESULT hres = COM_CALL(CheckFeatureSupport, pRenderer->mDx.pDevice, D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
    ASSERT(SUCCEEDED(hres));
    pRaytracing->mTier = opts5.RaytracingTier;

    *ppRaytracing = pRaytracing;
    return true;
}

void exitRaytracing(Renderer* pRenderer, Raytracing* pRaytracing)
{
    ASSERT(pRenderer);
    ASSERT(pRaytracing);

    COM_RELEASE(pRaytracing->prDevice);

    tf_free(pRaytracing);
}

static inline FORGE_CONSTEXPR D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE ToDXRASType(AccelerationStructureType type)
{
    return ACCELERATION_STRUCTURE_TYPE_BOTTOM == type ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL
                                                      : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
}

extern void AddSrv(Renderer*, struct DescriptorHeap*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, DxDescriptorID*);

void addAccelerationStructure(Raytracing* pRaytracing, const AccelerationStructureDesc* pDesc,
                              AccelerationStructure** ppAccelerationStructure)
{
    ASSERT(pRaytracing);
    ASSERT(pDesc);
    ASSERT(ppAccelerationStructure);

    size_t memSize = sizeof(AccelerationStructure);
    if (ACCELERATION_STRUCTURE_TYPE_BOTTOM == pDesc->mType)
    {
        memSize += pDesc->mBottom.mDescCount * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC);
    }

    AccelerationStructure* pAS = (AccelerationStructure*)tf_calloc(1, memSize);
    ASSERT(pAS);

    pAS->mFlags = util_to_dx_acceleration_structure_build_flags(pDesc->mFlags);
    pAS->mType = ToDXRASType(pDesc->mType);

    uint32_t scratchBufferSize = 0;

    if (ACCELERATION_STRUCTURE_TYPE_BOTTOM == pDesc->mType)
    {
        pAS->mDescCount = pDesc->mBottom.mDescCount;
        pAS->pGeometryDescs = (D3D12_RAYTRACING_GEOMETRY_DESC*)(pAS + 1); //-V1027
        for (uint32_t j = 0; j < pAS->mDescCount; ++j)
        {
            AccelerationStructureGeometryDesc* pGeom = &pDesc->mBottom.pGeometryDescs[j];
            D3D12_RAYTRACING_GEOMETRY_DESC*    pGeomD3D12 = &pAS->pGeometryDescs[j];

            pGeomD3D12->Flags = util_to_dx_geometry_flags(pGeom->mFlags);

            if (pGeom->mIndexCount)
            {
                ASSERT(pGeom->pIndexBuffer);
                pGeomD3D12->Triangles.IndexBuffer = pGeom->pIndexBuffer->mDx.mGpuAddress + pGeom->mIndexOffset;
                pGeomD3D12->Triangles.IndexCount = pGeom->mIndexCount;
                pGeomD3D12->Triangles.IndexFormat = (pGeom->mIndexType == INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT);
            }

            ASSERT(pGeom->pVertexBuffer);
            ASSERT(pGeom->mVertexCount);

            pGeomD3D12->Triangles.VertexBuffer.StartAddress = pGeom->pVertexBuffer->mDx.mGpuAddress + pGeom->mVertexOffset;
            pGeomD3D12->Triangles.VertexBuffer.StrideInBytes = pGeom->mVertexStride;
            pGeomD3D12->Triangles.VertexCount = pGeom->mVertexCount;
            pGeomD3D12->Triangles.VertexFormat = (DXGI_FORMAT)TinyImageFormat_ToDXGI_FORMAT(pGeom->mVertexFormat);
            /*
            Format of the vertices in VertexBuffer. Must be one of the following:

            DXGI_FORMAT_R32G32_FLOAT - third component is assumed 0
            DXGI_FORMAT_R32G32B32_FLOAT
            DXGI_FORMAT_R16G16_FLOAT - third component is assumed 0
            DXGI_FORMAT_R16G16B16A16_FLOAT - A16 component is ignored, other data can be packed there, such as setting vertex stride to 6
            bytes. DXGI_FORMAT_R16G16_SNORM - third component is assumed 0 DXGI_FORMAT_R16G16B16A16_SNORM - A16 component is ignored, other
            data can be packed there, such as setting vertex stride to 6 bytes. Tier 1.1 devices support the following additional formats:

            DXGI_FORMAT_R16G16B16A16_UNORM - A16 component is ignored, other data can be packed there, such as setting vertex stride to 6
            bytes DXGI_FORMAT_R16G16_UNORM - third component assumed 0 DXGI_FORMAT_R10G10B10A2_UNORM - A2 component is ignored, stride must
            be 4 bytes DXGI_FORMAT_R8G8B8A8_UNORM - A8 component is ignored, other data can be packed there, such as setting vertex stride
            to 3 bytes DXGI_FORMAT_R8G8_UNORM - third component assumed 0 DXGI_FORMAT_R8G8B8A8_SNORM - A8 component is ignored, other data
            can be packed there, such as setting vertex stride to 3 bytes DXGI_FORMAT_R8G8_SNORM - third component assumed 0
            */
            ASSERT(DXGI_FORMAT_R32G32_FLOAT == pGeomD3D12->Triangles.VertexFormat ||
                   DXGI_FORMAT_R32G32B32_FLOAT == pGeomD3D12->Triangles.VertexFormat ||
                   DXGI_FORMAT_R16G16_FLOAT == pGeomD3D12->Triangles.VertexFormat ||
                   DXGI_FORMAT_R16G16B16A16_FLOAT == pGeomD3D12->Triangles.VertexFormat ||
                   DXGI_FORMAT_R16G16_SNORM == pGeomD3D12->Triangles.VertexFormat ||
                   DXGI_FORMAT_R16G16B16A16_SNORM == pGeomD3D12->Triangles.VertexFormat ||
                   ((pRaytracing->mTier > D3D12_RAYTRACING_TIER_1_0)
                        ? (DXGI_FORMAT_R16G16B16A16_UNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R16G16_UNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R10G10B10A2_UNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R8G8B8A8_UNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R8G8_UNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R8G8B8A8_SNORM == pGeomD3D12->Triangles.VertexFormat ||
                           DXGI_FORMAT_R8G8_SNORM == pGeomD3D12->Triangles.VertexFormat)
                        : false));
        }
        /************************************************************************/
        // Get the size requirement for the Acceleration Structures
        /************************************************************************/
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildDesc = {
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .Flags = pAS->mFlags,
            .NumDescs = pAS->mDescCount,
            .pGeometryDescs = pAS->pGeometryDescs,
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        };
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = { 0 };
        hook_GetRaytracingAccelerationStructurePrebuildInfo(pRaytracing->prDevice, &prebuildDesc, &info);

        /************************************************************************/
        // Allocate Acceleration Structure Buffer
        /************************************************************************/
        BufferDesc bufferDesc = {
            .mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER,
            .mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY,
            .mFlags = BUFFER_CREATION_FLAG_OWN_MEMORY_BIT | BUFFER_CREATION_FLAG_NO_DESCRIPTOR_VIEW_CREATION,
            .mStructStride = 0,
            .mFirstElement = 0,
            .mElementCount = (uint32_t)(info.ResultDataMaxSizeInBytes / sizeof(UINT32)),
            .mSize = info.ResultDataMaxSizeInBytes,
            .mStartState = RESOURCE_STATE_ACCELERATION_STRUCTURE_WRITE,
        };
        addBuffer(pRaytracing->pRenderer, &bufferDesc, &pAS->pASBuffer);
        /************************************************************************/
        // Store the scratch buffer size so user can create the scratch buffer accordingly
        /************************************************************************/
        scratchBufferSize = (UINT)info.ScratchDataSizeInBytes > scratchBufferSize ? (UINT)info.ScratchDataSizeInBytes : scratchBufferSize;
    }
    else
    {
        pAS->mDescCount = pDesc->mTop.mDescCount;
        /************************************************************************/
        // Get the size requirement for the Acceleration Structures
        /************************************************************************/
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildDesc = {
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .Flags = util_to_dx_acceleration_structure_build_flags(pDesc->mFlags),
            .NumDescs = pDesc->mTop.mDescCount,
            .pGeometryDescs = NULL,
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        };

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = { 0 };
        hook_GetRaytracingAccelerationStructurePrebuildInfo(pRaytracing->prDevice, &prebuildDesc, &info);

        /************************************************************************/
        /*  Construct buffer with instances descriptions                        */
        /************************************************************************/
        D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs = NULL;
        arrsetlen(instanceDescs, pDesc->mTop.mDescCount);
        for (uint32_t i = 0; i < pDesc->mTop.mDescCount; ++i)
        {
            AccelerationStructureInstanceDesc* pInst = &pDesc->mTop.pInstanceDescs[i];
            ASSERT(pInst->pBottomAS);

            const Buffer* pASBuffer = pInst->pBottomAS->pASBuffer;
            instanceDescs[i] = (D3D12_RAYTRACING_INSTANCE_DESC){
                .AccelerationStructure = COM_CALL(GetGPUVirtualAddress, pASBuffer->mDx.pResource),
                .Flags = util_to_dx_instance_flags(pInst->mFlags),
                .InstanceContributionToHitGroupIndex = pInst->mInstanceContributionToHitGroupIndex,
                .InstanceID = pInst->mInstanceID,
                .InstanceMask = pInst->mInstanceMask,
            };

            memcpy(instanceDescs[i].Transform, pInst->mTransform, sizeof(float[12])); //-V595
        }

        BufferDesc instanceDesc = {
            .mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU,
            .mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT,
            .mSize = arrlenu(instanceDescs) * sizeof(instanceDescs[0]),
        };
        addBuffer(pRaytracing->pRenderer, &instanceDesc, &pAS->pInstanceDescBuffer);
        if (arrlen(instanceDescs))
        {
            memcpy(pAS->pInstanceDescBuffer->pCpuMappedAddress, instanceDescs, instanceDesc.mSize);
        }
        arrfree(instanceDescs);
        /************************************************************************/
        // Allocate Acceleration Structure Buffer
        /************************************************************************/
        BufferDesc bufferDesc = {
            .mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER_RAW | DESCRIPTOR_TYPE_BUFFER_RAW,
            .mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY,
            .mFlags = BUFFER_CREATION_FLAG_OWN_MEMORY_BIT,
            .mStructStride = 0,
            .mFirstElement = 0,
            .mElementCount = (uint32_t)(info.ResultDataMaxSizeInBytes / sizeof(UINT32)),
            .mSize = info.ResultDataMaxSizeInBytes,
            .mStartState = RESOURCE_STATE_ACCELERATION_STRUCTURE_WRITE,
        };
        addBuffer(pRaytracing->pRenderer, &bufferDesc, &pAS->pASBuffer);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Format = DXGI_FORMAT_UNKNOWN,
            .RaytracingAccelerationStructure.Location = pAS->pASBuffer->mDx.mGpuAddress,
        };
        DxDescriptorID srv = pAS->pASBuffer->mDx.mDescriptors + pAS->pASBuffer->mDx.mSrvDescriptorOffset;
        AddSrv(pRaytracing->pRenderer, NULL, NULL, &srvDesc, &srv);

        scratchBufferSize = (UINT)info.ScratchDataSizeInBytes;
    }

    // Create scratch buffer
    BufferDesc scratchBufferDesc = {
        .mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER,
        .mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY,
        .mStartState = RESOURCE_STATE_COMMON,
        .mFlags = BUFFER_CREATION_FLAG_NO_DESCRIPTOR_VIEW_CREATION,
        .mSize = scratchBufferSize,
    };
    addBuffer(pRaytracing->pRenderer, &scratchBufferDesc, &pAS->pScratchBuffer);

    *ppAccelerationStructure = pAS;
}

void removeAccelerationStructure(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
    ASSERT(pRaytracing);
    ASSERT(pAccelerationStructure);

    removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pASBuffer);
    if (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL == pAccelerationStructure->mType)
    {
        removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pInstanceDescBuffer);
    }
    if (pAccelerationStructure->pScratchBuffer)
    {
        removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pScratchBuffer);
        pAccelerationStructure->pScratchBuffer = NULL;
    }

    tf_free(pAccelerationStructure);
}

void removeAccelerationStructureScratch(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
    if (!pAccelerationStructure->pScratchBuffer)
    {
        return;
    }
    removeBuffer(pRaytracing->pRenderer, pAccelerationStructure->pScratchBuffer);
    pAccelerationStructure->pScratchBuffer = NULL;
}

/************************************************************************/
// Raytracing Command Buffer Functions Implementation
/************************************************************************/
void cmdBuildAccelerationStructure(Cmd* pCmd, Raytracing* pRaytracing, RaytracingBuildASDesc* pDesc)
{
    UNREF_PARAM(pRaytracing);
    ASSERT(pDesc);

    ID3D12GraphicsCommandList4* dxrCmd = NULL;
    COM_CALL(QueryInterface, pCmd->mDx.pCmdList, IID_ARGS(ID3D12GraphicsCommandList4, &dxrCmd));
    ASSERT(dxrCmd);

    AccelerationStructure*                             as = pDesc->pAccelerationStructure;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type = as->mType;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
        .DestAccelerationStructureData = COM_CALL(GetGPUVirtualAddress, as->pASBuffer->mDx.pResource),
        .ScratchAccelerationStructureData = COM_CALL(GetGPUVirtualAddress, as->pScratchBuffer->mDx.pResource),
        .Inputs =
            (D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS){
                .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
                .Type = type,
                .Flags = as->mFlags,
                .pGeometryDescs = NULL,
                .NumDescs = as->mDescCount,
            },
    };
    if (type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
    {
        buildDesc.Inputs.pGeometryDescs = as->pGeometryDescs;
    }
    else if (type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
    {
        buildDesc.Inputs.InstanceDescs = COM_CALL(GetGPUVirtualAddress, as->pInstanceDescBuffer->mDx.pResource);
    }
    hook_BuildRaytracingAccelerationStructure(dxrCmd, &buildDesc, 0, NULL);

    if (pDesc->mIssueRWBarrier)
    {
        BufferBarrier barrier = { as->pASBuffer, RESOURCE_STATE_ACCELERATION_STRUCTURE_WRITE, RESOURCE_STATE_ACCELERATION_STRUCTURE_READ };
        cmdResourceBarrier(pCmd, 1, &barrier, 0, NULL, 0, NULL);
    }

    COM_CALL(Release, dxrCmd);
}

/************************************************************************/
// Utility Functions Implementation
/************************************************************************/
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS util_to_dx_acceleration_structure_build_flags(AccelerationStructureBuildFlags flags)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ret = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    if (flags & ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE)
        ret |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    return ret;
}

D3D12_RAYTRACING_GEOMETRY_FLAGS util_to_dx_geometry_flags(AccelerationStructureGeometryFlags flags)
{
    D3D12_RAYTRACING_GEOMETRY_FLAGS ret = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    if (flags & ACCELERATION_STRUCTURE_GEOMETRY_FLAG_OPAQUE)
        ret |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    if (flags & ACCELERATION_STRUCTURE_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)
        ret |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;

    return ret;
}

D3D12_RAYTRACING_INSTANCE_FLAGS util_to_dx_instance_flags(AccelerationStructureInstanceFlags flags)
{
    D3D12_RAYTRACING_INSTANCE_FLAGS ret = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_OPAQUE)
        ret |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
    if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE)
        ret |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    if (flags & ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE)
        ret |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;

    return ret;
}

void fillRaytracingDescriptorHandle(AccelerationStructure* pAccelerationStructure, DxDescriptorID* pOutId)
{
    *pOutId = pAccelerationStructure->pASBuffer->mDx.mDescriptors + pAccelerationStructure->pASBuffer->mDx.mSrvDescriptorOffset;
}
#else

bool initRaytracing(Renderer* pRenderer, Raytracing** ppRaytracing)
{
    UNREF_PARAM(pRenderer);
    UNREF_PARAM(ppRaytracing);
    return false;
}

void exitRaytracing(Renderer* pRenderer, Raytracing* pRaytracing)
{
    UNREF_PARAM(pRenderer);
    UNREF_PARAM(pRaytracing);
}

void addAccelerationStructure(Raytracing* pRaytracing, const AccelerationStructureDesc* pDesc,
                              AccelerationStructure** ppAccelerationStructure)
{
    UNREF_PARAM(pRaytracing);
    UNREF_PARAM(pDesc);
    UNREF_PARAM(ppAccelerationStructure);
}

void removeAccelerationStructure(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
    UNREF_PARAM(pRaytracing);
    UNREF_PARAM(pAccelerationStructure);
}

void removeAccelerationStructureScratch(Raytracing* pRaytracing, AccelerationStructure* pAccelerationStructure)
{
    UNREF_PARAM(pRaytracing);
    UNREF_PARAM(pAccelerationStructure);
}

void cmdBuildAccelerationStructure(Cmd* pCmd, Raytracing* pRaytracing, RaytracingBuildASDesc* pDesc)
{
    UNREF_PARAM(pCmd);
    UNREF_PARAM(pRaytracing);
    UNREF_PARAM(pDesc);
}

#endif

#endif
