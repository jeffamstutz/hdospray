//
// Copyright 2018 Intel
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/hdOSPRay/mesh.h"
#include "pxr/imaging/glf/glew.h"
#include "pxr/imaging/hdOSPRay/config.h"
#include "pxr/imaging/hdOSPRay/context.h"
#include "pxr/imaging/hdOSPRay/instancer.h"
#include "pxr/imaging/hdOSPRay/material.h"
#include "pxr/imaging/hdOSPRay/renderParam.h"
#include "pxr/imaging/hdOSPRay/renderPass.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include "pxr/imaging/hdSt/drawItem.h"
#include "pxr/imaging/hdSt/geometricShader.h"
#include "pxr/imaging/hdSt/material.h"

#include "ospcommon/AffineSpace.h"

PXR_NAMESPACE_OPEN_SCOPE
// #define PINGY(x) { std::cout << __LINE__ << std::string(x) << std::endl; } 
#define PINGY(x) { }

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(
    HdOSPRayTokens,
    (st)
);
// clang-format on

// static std::mutex g_mutex;

// USD forces warnings when converting ospcommon::affine3f to osp::affine3f
const osp::affine3f identity({ 1, 0, 0, 0, 1, 0, 0, 0, 1 });

HdOSPRayMesh::HdOSPRayMesh(SdfPath const& id, SdfPath const& instancerId)
    : HdMesh(id, instancerId)
    , _ospMesh(nullptr)
    , _instanceModel(nullptr)
    , _adjacencyValid(false)
    , _normalsValid(false)
    , _refined(false)
    , _smoothNormals(false)
    , _doubleSided(false)
    , _cullStyle(HdCullStyleDontCare)
{
}

void
HdOSPRayMesh::Finalize(HdRenderParam* renderParam)
{
    // _ospInstances.clear();
}

HdDirtyBits
HdOSPRayMesh::GetInitialDirtyBitsMask() const
{
    // The initial dirty bits control what data is available on the first
    // run through _PopulateMesh(), so it should list every data item
    // that _PopulateMesh requests.
    int mask = HdChangeTracker::Clean | HdChangeTracker::InitRepr
           | HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology
           | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility
           | HdChangeTracker::DirtyCullStyle | HdChangeTracker::DirtyDoubleSided
           | HdChangeTracker::DirtyDisplayStyle
           | HdChangeTracker::DirtySubdivTags | HdChangeTracker::DirtyPrimvar
           | HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyInstanceIndex
           | HdChangeTracker::AllDirty // this magic bit seems to trigger
                                       // materials... bah
           ;

    return (HdDirtyBits)mask;
}

HdDirtyBits
HdOSPRayMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdOSPRayMesh::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
    TF_UNUSED(dirtyBits);

    // Create an empty repr.
    _ReprVector::iterator it = std::find_if(_reprs.begin(), _reprs.end(),
                                            _ReprComparator(reprToken));
    if (it == _reprs.end()) {
        _reprs.emplace_back(reprToken, HdReprSharedPtr());
    }
}

static bool
_IsEnabledForceQuadrangulate()
{
    static bool enabled = HdOSPRayConfig::GetInstance().forceQuadrangulate == 1;
    return enabled;
}

bool
HdOSPRayMesh::_UseQuadIndices(const HdRenderIndex& renderIndex,
                              HdMeshTopology const& topology) const
{
    // We should never quadrangulate for subdivision schemes
    // which refine to triangles (like Loop)
    if (topology.GetScheme() == PxOsdOpenSubdivTokens->loop)
        return false;

    // According to HdSt _ospMesh.cpp, always use quads on surfaces with ptex
    const HdOSPRayMaterial* material = static_cast<const HdOSPRayMaterial*>(
           renderIndex.GetSprim(HdPrimTypeTokens->material, GetMaterialId()));
    if (material && material->HasPtex())
        return true;

    // Fallback to the environment variable, which allows forcing of
    // quadrangulation for debugging/testing.
    return _IsEnabledForceQuadrangulate();
}

void
HdOSPRayMesh::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
                   HdDirtyBits* dirtyBits, TfToken const& reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    //OSPRay calls are not thread safe.  All osp calls should be mutex guarded.
    // std::lock_guard<std::mutex> lock(g_mutex);
    // g_mutex.lock();
    // std::lock_guard<std::mutex> lock(HdOSPRayConfig::GetMutableInstance().ospMutex);

    // XXX: Meshes can have multiple reprs; this is done, for example, when
    // the drawstyle specifies different rasterizing modes between front faces
    // and back faces. With raytracing, this concept makes less sense, but
    // combining semantics of two HdMeshReprDesc is tricky in the general case.
    // For now, HdOSPRayMesh only respects the first desc; this should be fixed.
    _MeshReprConfig::DescArray descs = _GetReprDesc(reprToken);
    const HdMeshReprDesc& desc = descs[0];

    // Pull top-level OSPRay state out of the render param.
    HdOSPRayRenderParam* ospRenderParam
           = static_cast<HdOSPRayRenderParam*>(renderParam);
    OSPModel model = ospRenderParam->GetOSPRayModel();
    OSPRenderer renderer = ospRenderParam->GetOSPRayRenderer();

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        _SetMaterialId(sceneDelegate->GetRenderIndex().GetChangeTracker(),
                       sceneDelegate->GetMaterialId(GetId()));
    }

    // Create ospray geometry objects.
     _PopulateOSPMesh(sceneDelegate, model, renderer, dirtyBits, desc,
                      ospRenderParam);

    if (*dirtyBits & HdChangeTracker::DirtyTopology) {
        // TODO: update material here?
    }
}

void
HdOSPRayMesh::_UpdatePrimvarSources(HdSceneDelegate* sceneDelegate,
                                    HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();
    SdfPath const& id = GetId();

    // Update _primvarSourceMap, our local cache of raw primvar data.
    // This function pulls data from the scene delegate, but defers processing.
    //
    // While iterating primvars, we skip "points" (vertex positions) because
    // the points primvar is processed by _PopulateMesh. We only call
    // GetPrimvar on primvars that have been marked dirty.
    //
    // Currently, hydra doesn't have a good way of communicating changes in
    // the set of primvars, so we only ever add and update to the primvar set.

    HdPrimvarDescriptorVector primvars;
    for (size_t i = 0; i < HdInterpolationCount; ++i) {
        HdInterpolation interp = static_cast<HdInterpolation>(i);
        primvars = GetPrimvarDescriptors(sceneDelegate, interp);
        for (HdPrimvarDescriptor const& pv : primvars) {

            // Points are handled outside _UpdatePrimvarSources
            if (pv.name == HdTokens->points)
                continue;

            auto value = sceneDelegate->Get(id, pv.name);

            // texcoords
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id,
                                                HdOSPRayTokens->st)) {
                if (value.IsHolding<VtVec2fArray>()) {
                    _texcoords = value.Get<VtVec2fArray>();
                }
            }

            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id,
                                                HdTokens->color)) {
                if (value.IsHolding<VtVec4fArray>())
                    _colors = value.Get<VtVec4fArray>();
            }
        }
    }
}

void
HdOSPRayMesh::_PopulateOSPMesh(HdSceneDelegate* sceneDelegate, OSPModel model,
                               OSPRenderer renderer, HdDirtyBits* dirtyBits,
                               HdMeshReprDesc const& desc,
                               HdOSPRayRenderParam* renderParam)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    ////////////////////////////////////////////////////////////////////////
    // 1. Pull scene data.

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue value = sceneDelegate->Get(id, HdTokens->points);
        _points = value.Get<VtVec3fArray>();
        if (_points.size() > 0)
            _normalsValid = false;
    }

    if (HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        HdDisplayStyle const displayStyle = sceneDelegate->GetDisplayStyle(id);
        _topology = HdMeshTopology(_topology, displayStyle.refineLevel);
    }

    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        _transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
    }

    if (HdChangeTracker::IsCullStyleDirty(*dirtyBits, id)) {
        _cullStyle = GetCullStyle(sceneDelegate);
    }
    if (HdChangeTracker::IsDoubleSidedDirty(*dirtyBits, id)) {
        _doubleSided = IsDoubleSided(sceneDelegate);
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals)
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->widths)
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->primvar)
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, id,
                                           HdOSPRayTokens->st)) {
        _UpdatePrimvarSources(sceneDelegate, *dirtyBits);
    }

    ////////////////////////////////////////////////////////////////////////
    // 2. Resolve drawstyles

    // The repr defines a set of geometry styles for drawing the mesh
    // (see hd/enums.h). We're ignoring points and wireframe for now, so
    // HdMeshGeomStyleSurf maps to subdivs and everything else maps to
    // HdMeshGeomStyleHull (coarse triangulated mesh).
    bool doRefine = (desc.geomStyle == HdMeshGeomStyleSurf);

    // If the subdivision scheme is "none", force us to not refine.
    doRefine
           = doRefine && (_topology.GetScheme() != PxOsdOpenSubdivTokens->none);

    // If the refine level is 0, triangulate instead of subdividing.
    doRefine = doRefine && (_topology.GetRefineLevel() > 0);

    // The repr defines whether we should compute smooth normals for this mesh:
    // per-vertex normals taken as an average of adjacent faces, and
    // interpolated smoothly across faces.
    _smoothNormals = !desc.flatShadingEnabled;

    // If the subdivision scheme is "none" or "bilinear", force us not to use
    // smooth normals.
    _smoothNormals = _smoothNormals
           && ((_topology.GetScheme() != PxOsdOpenSubdivTokens->none));

    // If the scene delegate has provided authored normals, force us to not use
    // smooth normals.
    bool authoredNormals = false;
    if (_primvarSourceMap.count(HdTokens->normals) > 0) {
        authoredNormals = true;
    }

    if (HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)
        && _topology.GetRefineLevel() > 0) {
        _topology.SetSubdivTags(sceneDelegate->GetSubdivTags(id));
    }

    ////////////////////////////////////////////////////////////////////////
    // 3. Populate ospray prototype object.

    // If the refine level changed or the mesh was recreated, we need to pass
    // the refine level into the ospray subdiv object.
    if (HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        if (doRefine) {
            // Pass the target number of uniform refinements to Embree.
            // Embree refinement is specified as the number of quads to generate
            // per edge, whereas hydra refinement is the number of recursive
            // splits, so we need to pass embree (2^refineLevel).

            _tessellationRate = (1 << _topology.GetRefineLevel());
            // XXX: As of Embree 2.9.0, rendering with tessellation level 1
            // (i.e. coarse mesh) results in weird normals, so force at least
            // one level of subdivision.
            if (_tessellationRate == 1) {
                _tessellationRate++;
            }
        }
    }


    PINGY();

    // If the topology has changed, or the value of doRefine has changed, we
    // need to create or recreate the OSPRay mesh object.
    // _GetInitialDirtyBits() ensures that the topology is dirty the first time
    // this function is called, so that the OSPRay mesh is always created.
    bool newMesh = false;
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)
        || doRefine != _refined) {
        newMesh = true;

        // Force the smooth normals code to rebuild the "normals" primvar the
        // next time smooth normals is enabled.
        _normalsValid = false;

        // When pulling a new topology, we don't want to overwrite the
        // refine level or subdiv tags, which are provided separately by the
        // scene delegate, so we save and restore them.
        PxOsdSubdivTags subdivTags = _topology.GetSubdivTags();
        int refineLevel = _topology.GetRefineLevel();
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), refineLevel);
        _topology.SetSubdivTags(subdivTags);
        _adjacencyValid = false;

        if (doRefine) {
            _ospMesh = _CreateOSPRaySubdivMesh();
            ospCommit(_ospMesh);

                HdMeshUtil meshUtil(&_topology, GetId());
                meshUtil.ComputeQuadIndices(&_quadIndices,
                                            &_quadPrimitiveParams);
                // Check if texcoords are provides as face varying.
                // XXX: (This code currently only cares about _texcoords, but
                // should be generalized to all primvars)
                bool faceVaryingTexcoord = false;
                HdPrimvarDescriptorVector faceVaryingPrimvars
                       = GetPrimvarDescriptors(sceneDelegate,
                                               HdInterpolationFaceVarying);
                for (HdPrimvarDescriptor const& pv : faceVaryingPrimvars) {
                    if (pv.name == "Texture_uv")
                        faceVaryingTexcoord = true;
                }

                if (faceVaryingTexcoord) {
                    TfToken buffName = HdOSPRayTokens->st;
                    VtValue buffValue = VtValue(_texcoords);
                    HdVtBufferSource buffer(buffName, buffValue);
                    VtValue quadPrimvar;

                    auto success
                           = meshUtil.ComputeQuadrangulatedFaceVaryingPrimvar(
                                  buffer.GetData(), buffer.GetNumElements(),
                                  buffer.GetTupleType().type, &quadPrimvar);
                    if (success && quadPrimvar.IsHolding<VtVec2fArray>()) {
                        _texcoords = quadPrimvar.Get<VtVec2fArray>();
                    } else {
                        std::cout
                               << "ERROR: could not quadrangulate face-varying "
                                  "data\n";
                    }

                    // usd stores texcoords in face indexed -> each quad has 4
                    // unique texcoords.
                    // let's try converting it to match our vertex indices
                    VtVec2fArray texcoords2;
                    texcoords2.resize(_points.size());
                    for (size_t q = 0; q < _quadIndices.size(); q++) {
                        for (int i = 0; i < 4; i++) {
                            // value at quadindex[q][i] maps to q*4+i texcoord;
                            const size_t tc1index = q * 4 + i;
                            const size_t tc2index = _quadIndices[q][i];
                            texcoords2[tc2index] = _texcoords[tc1index];
                        }
                    }
                    _texcoords = texcoords2;
                }
        } 
        _refined = doRefine;
    }
    PINGY();

    // If the subdiv tags changed or the _ospMesh was recreated, we need to update
    // the subdivision boundary mode.
    if (newMesh || HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) {
        if (doRefine) {
            TfToken const vertexRule
                   = _topology.GetSubdivTags().GetVertexInterpolationRule();

            // TODO: handle subdiv tags
            if (vertexRule == PxOsdOpenSubdivTokens->none) {
                // rtcSetBoundaryMode(_rtcMeshScene, _rtcMeshId,
                //     RTC_BOUNDARY_NONE);
            } else if (vertexRule == PxOsdOpenSubdivTokens->edgeOnly) {
                // rtcSetBoundaryMode(_rtcMeshScene, _rtcMeshId,
                //     RTC_BOUNDARY_EDGE_ONLY);
            } else if (vertexRule == PxOsdOpenSubdivTokens->edgeAndCorner) {
                // rtcSetBoundaryMode(_rtcMeshScene, _rtcMeshId,
                //     RTC_BOUNDARY_EDGE_AND_CORNER);
            } else {
                if (!vertexRule.IsEmpty()) {
                    TF_WARN("Unknown vertex interpolation rule: %s",
                            vertexRule.GetText());
                }
            }
        }
    }
    PINGY();

    // Update the smooth normals in steps:
    // 1. If the topology is dirty, update the adjacency table, a processed
    //    form of the topology that helps calculate smooth normals quickly.
    // 2. If the points are dirty, update the smooth normal buffer itself.
    _normalsValid = false;
    if (_smoothNormals && !_adjacencyValid) {
    PINGY();
        _adjacency.BuildAdjacencyTable(&_topology);
        _adjacencyValid = true;
        // If we rebuilt the adjacency table, force a rebuild of normals.
        _normalsValid = false;
    }
    if (_smoothNormals && !_normalsValid && !doRefine) {
    PINGY();
        _computedNormals = Hd_SmoothNormals::ComputeSmoothNormals(
               &_adjacency, _points.size(), _points.cdata());
        _normalsValid = true;
    }
    PINGY();

    // Create new OSP Mesh
    if (_instanceModel)
    {
        ospRelease(_instanceModel);
    }
    _instanceModel = ospNewModel();
    if (newMesh
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, id,
                                           HdOSPRayTokens->st)) {

        //    if (_primvarSourceMap.count(HdTokens->color) > 0) {
        //      auto& colorBuffer = _primvarSourceMap[HdTokens->color].data;
        //      if (colorBuffer.GetArraySize() &&
        //      colorBuffer.IsHolding<VtVec4fArray>()) {
        //        _colors = colorBuffer.Get<VtVec4fArray>();
        //        _colors.size() << "\n";
        //      }
        //    }

        const HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
        const HdOSPRayMaterial* material
               = static_cast<const HdOSPRayMaterial*>(renderIndex.GetSprim(
                      HdPrimTypeTokens->material, GetMaterialId()));

        if (!_refined) {
    PINGY();
            bool useQuads = _UseQuadIndices(renderIndex, _topology);

            if (useQuads) {
                _ospMesh = ospNewGeometry("quadmesh");

                HdMeshUtil meshUtil(&_topology, GetId());
                meshUtil.ComputeQuadIndices(&_quadIndices,
                                            &_quadPrimitiveParams);

                auto indices = ospNewData(_quadIndices.size(), OSP_INT4,
                                          _quadIndices.cdata(),
                                          OSP_DATA_SHARED_BUFFER);

                ospCommit(indices);
                ospSetData(_ospMesh, "index", indices);
                ospRelease(indices);

                // Check if texcoords are provides as face varying.
                // XXX: (This code currently only cares about _texcoords, but
                // should be generalized to all primvars)
                bool faceVaryingTexcoord = false;
                HdPrimvarDescriptorVector faceVaryingPrimvars
                       = GetPrimvarDescriptors(sceneDelegate,
                                               HdInterpolationFaceVarying);
                for (HdPrimvarDescriptor const& pv : faceVaryingPrimvars) {
                    if (pv.name == "Texture_uv")
                        faceVaryingTexcoord = true;
                }

                if (faceVaryingTexcoord) {
                    TfToken buffName = HdOSPRayTokens->st;
                    VtValue buffValue = VtValue(_texcoords);
                    HdVtBufferSource buffer(buffName, buffValue);
                    VtValue quadPrimvar;

                    auto success
                           = meshUtil.ComputeQuadrangulatedFaceVaryingPrimvar(
                                  buffer.GetData(), buffer.GetNumElements(),
                                  buffer.GetTupleType().type, &quadPrimvar);
                    if (success && quadPrimvar.IsHolding<VtVec2fArray>()) {
                        _texcoords = quadPrimvar.Get<VtVec2fArray>();
                    } else {
                        std::cout
                               << "ERROR: could not quadrangulate face-varying "
                                  "data\n";
                    }

                    // usd stores texcoords in face indexed -> each quad has 4
                    // unique texcoords.
                    // let's try converting it to match our vertex indices
                    VtVec2fArray texcoords2;
                    texcoords2.resize(_points.size());
                    for (size_t q = 0; q < _quadIndices.size(); q++) {
                        for (int i = 0; i < 4; i++) {
                            // value at quadindex[q][i] maps to q*4+i texcoord;
                            const size_t tc1index = q * 4 + i;
                            const size_t tc2index = _quadIndices[q][i];
                            texcoords2[tc2index] = _texcoords[tc1index];
                        }
                    }
                    _texcoords = texcoords2;
                }

            } else { // triangles
                _ospMesh = ospNewGeometry("trianglemesh");

                HdMeshUtil meshUtil(&_topology, GetId());
                meshUtil.ComputeTriangleIndices(&_triangulatedIndices,
                                                &_trianglePrimitiveParams);

                auto indices = ospNewData(_triangulatedIndices.size(), OSP_INT3,
                                          _triangulatedIndices.cdata(),
                                          OSP_DATA_SHARED_BUFFER);

                ospCommit(indices);
                ospSetData(_ospMesh, "index", indices);
                ospRelease(indices);

                // Check if texcoords are provides as face varying.
                // XXX: (This code currently only cares about _texcoords, but
                // should be generalized to all primvars)
                bool faceVaryingTexcoord = false;
                HdPrimvarDescriptorVector faceVaryingPrimvars
                       = GetPrimvarDescriptors(sceneDelegate,
                                               HdInterpolationFaceVarying);
                for (HdPrimvarDescriptor const& pv : faceVaryingPrimvars) {
                    if (pv.name == "Texture_uv")
                        faceVaryingTexcoord = true;
                }

                if (faceVaryingTexcoord) {
                    TfToken buffName = HdOSPRayTokens->st;
                    VtValue buffValue = VtValue(_texcoords);
                    HdVtBufferSource buffer(buffName, buffValue);
                    VtValue triangulatedPrimvar;

                    auto success
                           = meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                  buffer.GetData(), buffer.GetNumElements(),
                                  buffer.GetTupleType().type,
                                  &triangulatedPrimvar);
                    if (success
                        && triangulatedPrimvar.IsHolding<VtVec2fArray>()) {
                        _texcoords = triangulatedPrimvar.Get<VtVec2fArray>();
                    } else {
                        std::cout
                               << "ERROR: could not triangulate face-varying "
                                  "data\n";
                    }

                    // usd stores texcoords in face indexed -> each triangle has
                    // 3 unique texcoords. let's try converting it to match our
                    // vertex indices
                    VtVec2fArray texcoords2;
                    texcoords2.resize(_points.size());
                    for (size_t t = 0; t < _triangulatedIndices.size(); t++) {
                        for (int i = 0; i < 3; i++) {
                            // value at triangleIndex[t][i] maps to t*3+i
                            // texcoord;
                            const size_t tc1index = t * 3 + i;
                            const size_t tc2index = _triangulatedIndices[t][i];
                            texcoords2[tc2index] = _texcoords[tc1index];
                        }
                    }
                    _texcoords = texcoords2;
                }
            }
        }

        auto vertices = ospNewData(_points.size(), OSP_FLOAT3, _points.cdata(),
                                   OSP_DATA_SHARED_BUFFER);
        ospCommit(vertices);
        ospSetData(_ospMesh, "vertex", vertices);
        ospRelease(vertices);
    PINGY();

        if (_computedNormals.size()) {
            auto normals = ospNewData(_computedNormals.size(), OSP_FLOAT3,
                                      _computedNormals.cdata(),
                                      OSP_DATA_SHARED_BUFFER);
            ospSetData(_ospMesh, "vertex.normal", normals);
            ospRelease(normals);
        }
    PINGY();

        if (_colors.size() > 1) {
            // Carson: apparently colors are actually stored as a single color
            // value for entire object
            auto colors = ospNewData(_colors.size(), OSP_FLOAT4,
                                     _colors.cdata(), OSP_DATA_SHARED_BUFFER);
            ospSetData(_ospMesh, "vertex.color", colors);
            ospRelease(colors);
        }
    PINGY();

        if (_texcoords.size() > 1) {
            auto texcoords
                   = ospNewData(_texcoords.size(), OSP_FLOAT2,
                                _texcoords.cdata(), OSP_DATA_SHARED_BUFFER);
            ospCommit(texcoords);
            ospSetData(_ospMesh, "vertex.texcoord", texcoords);
            ospRelease(texcoords);
        }
    PINGY();

        OSPMaterial ospMaterial = nullptr;

        if (material && material->GetOSPRayMaterial()) {
            ospMaterial = material->GetOSPRayMaterial();
        } else {
            // Create new ospMaterial
            GfVec4f color(1.f);
            if (!_colors.empty())
                color = _colors[0];
            ospMaterial = HdOSPRayMaterial::CreateDefaultMaterial(color);
        }

        ospSetMaterial(_ospMesh, ospMaterial);
    PINGY();
        ospCommit(_ospMesh);
    PINGY();

        ospAddGeometry(_instanceModel,
                       _ospMesh); 
        ospRelease(_ospMesh);
    PINGY();
        ospCommit(_instanceModel);
    PINGY();
        renderParam->UpdateModelVersion();
    }
    PINGY();

    ////////////////////////////////////////////////////////////////////////
    // 4. Populate ospray instance objects.

    // If the _ospMesh is instanced, create one new instance per transform.
    // XXX: The current instancer invalidation tracking makes it hard for
    // HdOSPRay to tell whether transforms will be dirty, so this code
    // pulls them every frame.
    if (!GetInstancerId().IsEmpty()) {
    PINGY();
        // Retrieve instance transforms from the instancer.
        HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer* instancer = renderIndex.GetInstancer(GetInstancerId());
    PINGY();
        VtMatrix4dArray transforms
               = static_cast<HdOSPRayInstancer*>(instancer)
                        ->ComputeInstanceTransforms(GetId());
    PINGY();

        size_t newSize = transforms.size();
        _ospInstances.resize(newSize);
        for (size_t i = 0; i < newSize; i++) {
        // Create the new instance.
            auto instance = ospNewInstance(_instanceModel, identity);
            // Combine the local transform and the instance transform.
            GfMatrix4f matf = _transform * GfMatrix4f(transforms[i]);
            float* xfm = matf.GetArray();
            // convert aligned matrix to unalighned 4x3 matrix
            ospSet3f(instance, "xfm.l.vx", xfm[0], xfm[1], xfm[2]);
            ospSet3f(instance, "xfm.l.vy", xfm[4], xfm[5], xfm[6]);
            ospSet3f(instance, "xfm.l.vz", xfm[8], xfm[9], xfm[10]);
            ospSet3f(instance, "xfm.p", xfm[12], xfm[13], xfm[14]);
            ospCommit(instance);
            _ospInstances[i] = instance;
        }
    }
    // Otherwise, create our single instance (if necessary) and update
    // the transform (if necessary).
    else {
    PINGY();
        std::lock_guard<std::mutex> lock(HdOSPRayConfig::GetMutableInstance().ospMutex);
        _ospInstances.resize(0);
        auto instance = ospNewInstance(_instanceModel, identity);
        _ospInstances.push_back(instance);
        ospCommit(instance);
        // convert aligned matrix to unalighned 4x3 matrix
        float* xfm = _transform.GetArray();
        // convert aligned matrix to unalighned 4x3 matrix
        ospSet3f(instance, "xfm.l.vx", xfm[0], xfm[1], xfm[2]);
        ospSet3f(instance, "xfm.l.vy", xfm[4], xfm[5], xfm[6]);
        ospSet3f(instance, "xfm.l.vz", xfm[8], xfm[9], xfm[10]);
        ospSet3f(instance, "xfm.p", xfm[12], xfm[13], xfm[14]);
        ospCommit(instance);
    }
    PINGY();

    // Update visibility by pulling the object into/out of the model.
    if (_sharedData.visible) {
        std::lock_guard<std::mutex> lock(HdOSPRayConfig::GetMutableInstance().ospMutex);
        for (auto instance : _ospInstances) {
            HdOSPRayConfig::GetMutableInstance().ospInstances.push_back(instance);
        }
    } else {
        // TODO: ospRemove geometry?
    }

    // Clean all dirty bits.
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    PINGY();
}

void
HdOSPRayMesh::_UpdateDrawItemGeometricShader(HdSceneDelegate* sceneDelegate,
                                             HdStDrawItem* drawItem,
                                             const HdMeshReprDesc& desc,
                                             size_t drawItemIdForDesc)
{
    HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();

    // resolve geom style, cull style
    HdCullStyle cullStyle = desc.cullStyle;

    // if the repr doesn't have an opinion about cullstyle, use the
    // prim's default (it could also be DontCare, then renderPass's
    // cullStyle is going to be used).
    //
    // i.e.
    //   Repr CullStyle > Rprim CullStyle > RenderPass CullStyle
    //
    if (cullStyle == HdCullStyleDontCare) {
        cullStyle = _cullStyle;
    }

    // The edge geomstyles below are rasterized as lines.
    // See HdSt_GeometricShader::BindResources()
    // The batches need to be validated and rebuilt if necessary.
    renderIndex.GetChangeTracker().MarkBatchesDirty();
}

OSPGeometry
HdOSPRayMesh::_CreateOSPRaySubdivMesh()
{
    const PxOsdSubdivTags& subdivTags = _topology.GetSubdivTags();

    // The embree edge crease buffer expects ungrouped edges: a pair
    // of indices marking an edge and one weight per crease.
    // HdMeshTopology stores edge creases compactly. A crease length
    // buffer stores the number of indices per crease and groups the
    // crease index buffer, much like the face buffer groups the vertex index
    // buffer except that creases don't automatically close. Crease weights
    // can be specified per crease or per individual edge.
    //
    // For example, to add the edges [v0->v1@2.0f] and [v1->v2@2.0f],
    // HdMeshTopology might store length = [3], indices = [v0, v1, v2],
    // and weight = [2.0f], or it might store weight = [2.0f, 2.0f].
    //
    // This loop calculates the number of edge creases, in preparation for
    // unrolling the edge crease buffer below.
    VtIntArray const creaseLengths = subdivTags.GetCreaseLengths();
    int numEdgeCreases = 0;
    for (size_t i = 0; i < creaseLengths.size(); ++i) {
        numEdgeCreases += creaseLengths[i] - 1;
    }

    // For vertex creases, sanity check that the weights and indices
    // arrays are the same length.
    int numVertexCreases
           = static_cast<int>(subdivTags.GetCornerIndices().size());
    if (numVertexCreases
        != static_cast<int>(subdivTags.GetCornerWeights().size())) {
        TF_WARN("Mismatch between vertex crease indices and weights");
        numVertexCreases = 0;
    }

    // Populate an embree subdiv object.
    // _rtcMeshId = rtcNewSubdivisionMesh(scene, RTC_GEOMETRY_DEFORMABLE,
    //     // numFaces is the size of RTC_FACE_BUFFER, which contains
    //     // the number of indices for each face. This is equivalent to
    //     // HdMeshTopology's GetFaceVertexCounts().
    //     _topology.GetFaceVertexCounts().size(),
    //     // numEdges is the size of RTC_INDEX_BUFFER, which contains
    //     // the vertex indices for each face (grouped into faces by the
    //     // face buffer). This is equivalent to HdMeshTopology's
    //     // GetFaceVertexIndices(). Note this is more properly a count
    //     // of half-edges.
    //     _topology.GetFaceVertexIndices().size(),
    //     // numVertices is the size of RTC_VERTEX_BUFFER, or vertex
    //     // positions.
    //     _points.size(),
    //     // numEdgeCreases is the size of RTC_EDGE_CREASE_WEIGHT_BUFFER,
    //     // and half the size of RTC_EDGE_CREASE_INDEX_BUFFER. See
    //     // the calculation of numEdgeCreases above.
    //     numEdgeCreases,
    //     // numVertexCreases is the size of
    //     // RTC_VERTEX_CREASE_WEIGHT_BUFFER and _INDEX_BUFFER.
    //     numVertexCreases,
    //     // numHoles is the size of RTC_HOLE_BUFFER.
    //     _topology.GetHoleIndices().size());

    auto mesh = ospNewGeometry("subdivision");
    int numFaceVertices = _topology.GetFaceVertexCounts().size();
    int numIndices = _topology.GetFaceVertexIndices().size();
    int numVertices = _points.size();
    int numHoles = _topology.GetHoleIndices().size();

    // Fill the topology buffers.
    // rtcSetBuffer(scene, _rtcMeshId, RTC_FACE_BUFFER,
    //     _topology.GetFaceVertexCounts().cdata(), 0, sizeof(int));
    // rtcSetBuffer(scene, _rtcMeshId, RTC_INDEX_BUFFER,
    //     _topology.GetFaceVertexIndices().cdata(), 0, sizeof(int));
    // rtcSetBuffer(scene, _rtcMeshId, RTC_HOLE_BUFFER,
    //     _topology.GetHoleIndices().cdata(), 0, sizeof(int));

    auto vertices = ospNewData(numVertices, OSP_FLOAT3, _points.cdata());
    ospSetData(mesh, "vertex", vertices);
    ospRelease(vertices);
    auto faces = ospNewData(numFaceVertices, OSP_UINT,
                            _topology.GetFaceVertexCounts().cdata());
    ospSetData(mesh, "face", faces);
    ospRelease(faces);
    auto indices = ospNewData(numIndices, OSP_UINT,
                              _topology.GetFaceVertexIndices().cdata());
    ospSetData(mesh, "index", indices);
    ospRelease(indices);
    // TODO: set hole buffer
    GfVec4f color(1.f);
    if (!_colors.empty())
      color = _colors[0];
    std::vector<GfVec4f> colorDummy(_points.size(), color);
    auto colors = ospNewData(colorDummy.size(), OSP_FLOAT4, colorDummy.data());
    ospSetData(mesh, "color", colors);
    ospRelease(colors);
    // TODO: ospray subd appears to require color data... this should be fixed

    ospSet1f(mesh, "level", _tessellationRate);

    // If this topology has edge creases, unroll the edge crease buffer.
    if (numEdgeCreases > 0) {
        std::vector<unsigned int> ospCreaseIndices(numEdgeCreases*2);
        std::vector<float> ospCreaseWeights(numEdgeCreases);
    //     int *embreeCreaseIndices = static_cast<int*>(rtcMapBuffer(
    //         scene, _rtcMeshId, RTC_EDGE_CREASE_INDEX_BUFFER));
    //     float *embreeCreaseWeights = static_cast<float*>(rtcMapBuffer(
    //         scene, _rtcMeshId, RTC_EDGE_CREASE_WEIGHT_BUFFER));
        int ospEdgeIndex = 0;

        VtIntArray const creaseIndices = subdivTags.GetCreaseIndices();
        VtFloatArray const creaseWeights =
            subdivTags.GetCreaseWeights();

        bool weightPerCrease =
            (creaseWeights.size() == creaseLengths.size());

        // Loop through the creases; for each crease, loop through
        // the edges.
        int creaseIndexStart = 0;
        for (size_t i = 0; i < creaseLengths.size(); ++i) {
            int numEdges = creaseLengths[i] - 1;
            for(int j = 0; j < numEdges; ++j) {
                // Store the crease indices.
                ospCreaseIndices[2*ospEdgeIndex+0] =
                    creaseIndices[creaseIndexStart+j];
                ospCreaseIndices[2*ospEdgeIndex+1] =
                    creaseIndices[creaseIndexStart+j+1];

                // Store the crease weight.
                ospCreaseWeights[ospEdgeIndex] = weightPerCrease ?
                    creaseWeights[i] : creaseWeights[ospEdgeIndex];

                ospEdgeIndex++;
            }
            creaseIndexStart += creaseLengths[i];
        }

    //     rtcUnmapBuffer(scene, _rtcMeshId, RTC_EDGE_CREASE_INDEX_BUFFER);
    //     rtcUnmapBuffer(scene, _rtcMeshId, RTC_EDGE_CREASE_WEIGHT_BUFFER);

  auto edge_crease_indices = ospNewData(numEdgeCreases, OSP_UINT2, ospCreaseIndices.data());
  ospSetData(mesh, "edgeCrease.index", edge_crease_indices);
  ospRelease(edge_crease_indices);
  auto edge_crease_weights = ospNewData(numEdgeCreases, OSP_FLOAT, ospCreaseWeights.data());
  ospSetData(mesh, "edgeCrease.weight", edge_crease_weights);
  ospRelease(edge_crease_weights);
    }

    if (numVertexCreases > 0) {
  auto vertex_crease_indices = ospNewData(numVertexCreases, OSP_UINT, subdivTags.GetCornerIndices().cdata());
  ospSetData(mesh, "vertexCrease.index", vertex_crease_indices);
  ospRelease(vertex_crease_indices);
  auto vertex_crease_weights = ospNewData(numVertexCreases, OSP_FLOAT, subdivTags.GetCornerWeights().cdata());
  ospSetData(mesh, "vertexCrease.weight", vertex_crease_weights);
  ospRelease(vertex_crease_weights);
    //     rtcSetBuffer(scene, _rtcMeshId, RTC_VERTEX_CREASE_INDEX_BUFFER,
    //         subdivTags.GetCornerIndices().cdata(), 0, sizeof(int));
    //     rtcSetBuffer(scene, _rtcMeshId, RTC_VERTEX_CREASE_WEIGHT_BUFFER,
    //         subdivTags.GetCornerWeights().cdata(), 0, sizeof(float));
    }

    return mesh;
}

PXR_NAMESPACE_CLOSE_SCOPE
