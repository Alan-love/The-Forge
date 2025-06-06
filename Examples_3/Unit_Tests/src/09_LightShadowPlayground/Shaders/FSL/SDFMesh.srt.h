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

#ifndef baked_sdf_mesh_shadow_srt_h
#define baked_sdf_mesh_shadow_srt_h

BEGIN_SRT(BakedSDFMeshShadowSrtData)
#include "PersistentSet.h"
#include "PerFrameSet.h"
	BEGIN_SRT_SET(PerBatch)
#if FT_MULTIVIEW
		DECL_WTEXTURE(PerBatch, WTex2DArray(float4), gOutTexture)
		DECL_WTEXTURE(PerBatch, WTex2DArray(float2), gOutTextureF2)
#else
		DECL_WTEXTURE(PerBatch, WTex2D(float4), gOutTexture)
		DECL_WTEXTURE(PerBatch, WTex2D(float2), gOutTextureF2)
#endif
	END_SRT_SET(PerBatch)

#if FT_MULTIVIEW
	BEGIN_SRT_SET(PerDraw)
		DECL_CBUFFER(PerDraw, CBUFFER(SDFPerDrawData), gPerDrawSDFData)
    END_SRT_SET(PerDraw)
#endif
END_SRT(BakedSDFMeshShadowSrtData)

#endif /* baked_sdf_mesh_shadow_srt_h */

