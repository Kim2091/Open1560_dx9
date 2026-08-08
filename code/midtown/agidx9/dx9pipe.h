/*
    Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
    Copyright (C) 2020 Brick

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "agisdl/sdlpipe.h"

#include "dx9shader.h"

class agiDX9Context;
class agiDX9Rasterizer;

class agiDX9Pipeline final : public agiSDLPipeline
{
public:
    agiDX9Pipeline();
    ~agiDX9Pipeline();

    i32 BeginGfx() override;
    void EndGfx() override;

    void BeginFrame() override;
    void BeginScene() override;
    void EndScene() override;
    void EndFrame() override;

    RcOwner<agiTexDef> CreateTexDef() override;
    RcOwner<agiTexLut> CreateTexLut() override;
    RcOwner<DLP> CreateDLP() override;
    RcOwner<agiLight> CreateLight() override;
    RcOwner<agiLightModel> CreateLightModel() override;
    RcOwner<agiViewport> CreateViewport() override;
    RcOwner<agiBitmap> CreateBitmap() override;

    void CopyBitmap(i32 dst_x, i32 dst_y, agiBitmap* src, i32 src_x, i32 src_y, i32 width, i32 height) override;

    void ClearAll(i32 color) override;
    void ClearRect(i32 x, i32 y, i32 width, i32 height, u32 color) override;

    bool SupportsNativeTransform() const override
    {
        return true;
    }

    // Whether we are between BeginScene() and EndScene(), i.e. drawing the 3D world rather than
    // the HUD/menu overlay. Used by the submission census to tell real world geometry that is
    // still CPU-pretransformed apart from genuinely-2D interface drawing.
    bool IsInScene() const
    {
        return in_scene_ != 0;
    }

    void Init();

    agiDX9Context* Context() const
    {
        return dx9_context_.get();
    }

    agiDX9Rasterizer* Rast()
    {
        return rasterizer_.get();
    }

    // Pathway B. Returns null until BeginGfx() has successfully brought it up, and stays null
    // whenever -d3d9shaders is absent, the device lacks ps_3_0, or a shader failed to compile - so
    // every caller treats "no shader" as "use the fixed-function path", which is also the default.
    agiDX9WorldShader* WorldShader()
    {
        return world_shader_.IsValid() ? &world_shader_ : nullptr;
    }

private:
    agiDX9WorldShader world_shader_ {};

    Ptr<agiSurfaceDesc> CaptureScreen();

    Ptr<agiDX9Context> dx9_context_;
    Rc<agiDX9Rasterizer> rasterizer_;
    bool d3d_scene_active_ {};
};

Owner<agiPipeline> dx9CreatePipeline(i32 argc, char** argv);
