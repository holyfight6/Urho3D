//
// Copyright (c) 2008-2013 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "CollisionShape.h"
#include "Context.h"
#include "DebugRenderer.h"
#include "Drawable.h"
#include "Geometry.h"
#include "Log.h"
#include "Navigable.h"
#include "NavigationMesh.h"
#include "Profiler.h"
#include "Scene.h"
#include "StaticModel.h"
#include "TerrainPatch.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include "DebugNew.h"

namespace Urho3D
{

static const float DEFAULT_CELL_SIZE = 0.3f;
static const float DEFAULT_CELL_HEIGHT = 0.2f;
static const float DEFAULT_AGENT_HEIGHT = 2.0f;
static const float DEFAULT_AGENT_RADIUS = 0.6f;
static const float DEFAULT_AGENT_MAX_CLIMB = 0.9f;
static const float DEFAULT_AGENT_MAX_SLOPE = 45.0f;
static const float DEFAULT_REGION_MIN_SIZE = 8.0f;
static const float DEFAULT_REGION_MERGE_SIZE = 20.0f;
static const float DEFAULT_EDGE_MAX_LENGTH = 12.0f;
static const float DEFAULT_EDGE_MAX_ERROR = 1.3f;
static const float DEFAULT_DETAIL_SAMPLE_DISTANCE = 6.0f;
static const float DEFAULT_DETAIL_SAMPLE_MAX_ERROR = 1.0f;

static const int MAX_POLYS = 256;

/// Temporary data for building the Recast navigation mesh.
struct NavigationBuildData
{
    /// Construct.
    NavigationBuildData() :
        ctx_(new rcContext(false)),
        heightField_(0),
        compactHeightField_(0),
        contourSet_(0),
        polyMesh_(0),
        polyMeshDetail_(0)
    {
    }
    
    /// Destruct.
    ~NavigationBuildData()
    {
        delete(ctx_);
        rcFreeHeightField(heightField_);
        rcFreeCompactHeightfield(compactHeightField_);
        rcFreeContourSet(contourSet_);
        rcFreePolyMesh(polyMesh_);
        rcFreePolyMeshDetail(polyMeshDetail_);
        
        ctx_ = 0;
        heightField_ = 0;
        compactHeightField_ = 0;
        contourSet_ = 0;
        polyMesh_ = 0;
        polyMeshDetail_ = 0;
    }
    
    /// World-space bounding box of the navigation mesh.
    BoundingBox worldBoundingBox_;
    /// Build phase vertices.
    PODVector<Vector3> vertices_;
    /// Build phase triangle indices.
    PODVector<int> indices_;
    /// Recast context.
    rcContext* ctx_;
    /// Recast heightfield.
    rcHeightfield* heightField_;
    /// Recast compact heightfield.
    rcCompactHeightfield* compactHeightField_;
    /// Recast contour set.
    rcContourSet* contourSet_;
    /// Recast poly mesh.
    rcPolyMesh* polyMesh_;
    /// Recast detail poly mesh.
    rcPolyMeshDetail* polyMeshDetail_;
};

OBJECTTYPESTATIC(NavigationMesh);

NavigationMesh::NavigationMesh(Context* context) :
    Component(context),
    cellSize_(DEFAULT_CELL_SIZE),
    cellHeight_(DEFAULT_CELL_HEIGHT),
    agentHeight_(DEFAULT_AGENT_HEIGHT),
    agentRadius_(DEFAULT_AGENT_RADIUS),
    agentMaxClimb_(DEFAULT_AGENT_MAX_CLIMB),
    agentMaxSlope_(DEFAULT_AGENT_MAX_SLOPE),
    regionMinSize_(DEFAULT_REGION_MIN_SIZE),
    regionMergeSize_(DEFAULT_REGION_MERGE_SIZE),
    edgeMaxLength_(DEFAULT_EDGE_MAX_LENGTH),
    edgeMaxError_(DEFAULT_EDGE_MAX_ERROR),
    detailSampleDistance_(DEFAULT_DETAIL_SAMPLE_DISTANCE),
    detailSampleMaxError_(DEFAULT_DETAIL_SAMPLE_MAX_ERROR),
    navMesh_(0),
    navMeshQuery_(0),
    queryFilter_(new dtQueryFilter())
{
}

NavigationMesh::~NavigationMesh()
{
    ReleaseNavMesh();
    
    delete queryFilter_;
    queryFilter_ = 0;
}

void NavigationMesh::RegisterObject(Context* context)
{
    context->RegisterFactory<NavigationMesh>();
    
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Cell Size", GetCellSize, SetCellSize, float, DEFAULT_CELL_SIZE, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Cell Height", GetCellHeight, SetCellHeight, float, DEFAULT_CELL_HEIGHT, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Agent Height", GetAgentHeight, SetAgentHeight, float, DEFAULT_AGENT_HEIGHT, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Agent Radius", GetAgentRadius, SetAgentRadius, float, DEFAULT_AGENT_RADIUS, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Agent Max Climb", GetAgentMaxClimb, SetAgentMaxClimb, float, DEFAULT_AGENT_MAX_CLIMB, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Agent Max Slope", GetAgentMaxSlope, SetAgentMaxSlope, float, DEFAULT_AGENT_MAX_SLOPE, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Region Min Size", GetRegionMinSize, SetRegionMinSize, float, DEFAULT_REGION_MIN_SIZE, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Region Merge Size", GetRegionMergeSize, SetRegionMergeSize, float, DEFAULT_REGION_MERGE_SIZE, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Edge Max Length", GetEdgeMaxLength, SetEdgeMaxLength, float, DEFAULT_EDGE_MAX_LENGTH, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Edge Max Error", GetEdgeMaxError, SetEdgeMaxError, float, DEFAULT_EDGE_MAX_ERROR, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Detail Sample Distance", GetDetailSampleDistance, SetDetailSampleDistance, float, DEFAULT_DETAIL_SAMPLE_DISTANCE, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_FLOAT, "Detail Sample Max Error", GetDetailSampleMaxError, SetDetailSampleMaxError, float, DEFAULT_DETAIL_SAMPLE_MAX_ERROR, AM_DEFAULT);
    REF_ACCESSOR_ATTRIBUTE(NavigationMesh, VAR_BUFFER, "Navigation Data", GetNavigationDataAttr, SetNavigationDataAttr, PODVector<unsigned char>, Variant::emptyBuffer, AM_FILE | AM_NOEDIT);
}

void NavigationMesh::DrawDebugGeometry(DebugRenderer* debug, bool depthTest)
{
    if (!navMesh_ || !node_)
        return;
    
    const dtNavMesh* navMesh = navMesh_;
    const dtMeshTile* tile = navMesh->getTile(0);
    if (!tile)
        return;
    
    const Matrix3x4& worldTransform = node_->GetWorldTransform();
    
    for (int i = 0; i < tile->header->polyCount; ++i)
    {
        dtPoly* poly = tile->polys + i;
        for (unsigned j = 0; j < poly->vertCount; ++j)
        {
            debug->AddLine(
                worldTransform * *reinterpret_cast<const Vector3*>(&tile->verts[poly->verts[j] * 3]),
                worldTransform * *reinterpret_cast<const Vector3*>(&tile->verts[poly->verts[(j + 1) % poly->vertCount] * 3]),
                Color::YELLOW,
                depthTest
            );
        }
    }
}

void NavigationMesh::SetCellSize(float size)
{
    cellSize_ = Max(size, M_EPSILON);
}

void NavigationMesh::SetCellHeight(float height)
{
    cellHeight_ = Max(height, M_EPSILON);
}

void NavigationMesh::SetAgentHeight(float height)
{
    agentHeight_ = Max(height, M_EPSILON);
}

void NavigationMesh::SetAgentRadius(float radius)
{
    agentRadius_ = Max(radius, M_EPSILON);
}

void NavigationMesh::SetAgentMaxClimb(float maxClimb)
{
    agentMaxClimb_ = Max(maxClimb, M_EPSILON);
}

void NavigationMesh::SetAgentMaxSlope(float maxSlope)
{
    agentMaxSlope_ = Max(maxSlope, 0.0f);
}

void NavigationMesh::SetRegionMinSize(float size)
{
    regionMinSize_ = Max(size, M_EPSILON);
}

void NavigationMesh::SetRegionMergeSize(float size)
{
    regionMergeSize_ = Max(size, M_EPSILON);
}

void NavigationMesh::SetEdgeMaxLength(float length)
{
    edgeMaxLength_ = Max(length, M_EPSILON);
}

void NavigationMesh::SetEdgeMaxError(float error)
{
    edgeMaxError_ = Max(error, M_EPSILON);
}

void NavigationMesh::SetDetailSampleDistance(float distance)
{
    detailSampleDistance_ = Max(distance, M_EPSILON);
}

void NavigationMesh::SetDetailSampleMaxError(float error)
{
    detailSampleMaxError_ = Max(error, M_EPSILON);
}

bool NavigationMesh::Build()
{
    PROFILE(BuildNavigationMesh);
    
    ReleaseNavMesh();
    
    if (!node_)
        return false;
    
    if (!node_->GetWorldScale().Equals(Vector3::ONE))
        LOGWARNING("Navigation mesh root node has scaling. Agent parameters may not work as intended");
    
    NavigationBuildData build;
    
    {
        PROFILE(CollectNavigationGeometry);
        // Get Navigable components from child nodes, not from whole scene. This makes it theoretically possible to partition
        // the scene into several navigation meshes
        PODVector<Navigable*> navigables;
        node_->GetComponents<Navigable>(navigables, true);
        
        HashSet<Node*> processedNodes;
        for (unsigned i = 0; i < navigables.Size(); ++i)
        {
            if (navigables[i]->IsEnabledEffective())
                CollectGeometries(build, navigables[i]->GetNode(), processedNodes, navigables[i]->IsRecursive());
        }
        
        LOGDEBUG("Navigation mesh has " + String(build.vertices_.Size()) + " vertices and " + String(build.indices_.Size()) +
            " indices");
    }
    
    if (!build.vertices_.Size() || !build.indices_.Size())
        return true; // Nothing to do
    
    {
        PROFILE(ProcessNavigationGeometry);
        
        rcConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.cs = cellSize_;
        cfg.ch = cellHeight_;
        cfg.walkableSlopeAngle = agentMaxSlope_;
        cfg.walkableHeight = (int)ceilf(agentHeight_ / cfg.ch);
        cfg.walkableClimb = (int)floorf(agentMaxClimb_ / cfg.ch);
        cfg.walkableRadius = (int)ceilf(agentRadius_ / cfg.cs);
        cfg.maxEdgeLen = (int)(edgeMaxLength_ / cellSize_);
        cfg.maxSimplificationError = edgeMaxError_;
        cfg.minRegionArea = (int)sqrtf(regionMinSize_);
        cfg.mergeRegionArea = (int)sqrtf(regionMergeSize_);
        cfg.maxVertsPerPoly = 6;
        cfg.detailSampleDist = detailSampleDistance_ < 0.9f ? 0.0f : cellSize_ * detailSampleDistance_;
        cfg.detailSampleMaxError = cellHeight_ * detailSampleMaxError_;
        
        rcVcopy(cfg.bmin, &build.worldBoundingBox_.min_.x_);
        rcVcopy(cfg.bmax, &build.worldBoundingBox_.max_.x_);
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
        
        build.heightField_ = rcAllocHeightfield();
        if (!build.heightField_)
        {
            LOGERROR("Could not allocate heightfield");
            return false;
        }
        
        if (!rcCreateHeightfield(build.ctx_, *build.heightField_, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        {
            LOGERROR("Could not create heightfield");
            return false;
        }
        
        unsigned numTriangles = build.indices_.Size() / 3;
        SharedArrayPtr<unsigned char> triAreas(new unsigned char[numTriangles]);
        memset(triAreas.Get(), 0, numTriangles);
        
        rcMarkWalkableTriangles(build.ctx_, cfg.walkableSlopeAngle, &build.vertices_[0].x_, build.vertices_.Size(),
            &build.indices_[0], numTriangles, triAreas.Get());
        rcRasterizeTriangles(build.ctx_, &build.vertices_[0].x_, build.vertices_.Size(), &build.indices_[0],
            triAreas.Get(), numTriangles, *build.heightField_, cfg.walkableClimb);
        rcFilterLowHangingWalkableObstacles(build.ctx_, cfg.walkableClimb, *build.heightField_);
        rcFilterLedgeSpans(build.ctx_, cfg.walkableHeight, cfg.walkableClimb, *build.heightField_);
        rcFilterWalkableLowHeightSpans(build.ctx_, cfg.walkableHeight, *build.heightField_);
        
        build.compactHeightField_ = rcAllocCompactHeightfield();
        if (!build.compactHeightField_)
        {
            LOGERROR("Could not allocate create compact heightfield");
            return false;
        }
        if (!rcBuildCompactHeightfield(build.ctx_, cfg.walkableHeight, cfg.walkableClimb, *build.heightField_,
            *build.compactHeightField_))
        {
            LOGERROR("Could not build compact heightfield");
            return false;
        }
        if (!rcErodeWalkableArea(build.ctx_, cfg.walkableRadius, *build.compactHeightField_))
        {
            LOGERROR("Could not erode compact heightfield");
            return false;
        }
        if (!rcBuildDistanceField(build.ctx_, *build.compactHeightField_))
        {
            LOGERROR("Could not build distance field");
            return false;
        }
        if (!rcBuildRegions(build.ctx_, *build.compactHeightField_, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        {
            LOGERROR("Could not build regions");
            return false;
        }
        
        build.contourSet_ = rcAllocContourSet();
        if (!build.contourSet_)
        {
            LOGERROR("Could not allocate contour set");
            return false;
        }
        if (!rcBuildContours(build.ctx_, *build.compactHeightField_, cfg.maxSimplificationError, cfg.maxEdgeLen,
            *build.contourSet_))
        {
            LOGERROR("Could not create contours");
            return false;
        }
        
        build.polyMesh_ = rcAllocPolyMesh();
        if (!build.polyMesh_)
        {
            LOGERROR("Could not allocate poly mesh");
            return false;
        }
        if (!rcBuildPolyMesh(build.ctx_, *build.contourSet_, cfg.maxVertsPerPoly, *build.polyMesh_))
        {
            LOGERROR("Could not triangulate contours");
            return false;
        }
        
        build.polyMeshDetail_ = rcAllocPolyMeshDetail();
        if (!build.polyMeshDetail_)
        {
            LOGERROR("Could not allocate detail mesh");
            return false;
        }
        if (!rcBuildPolyMeshDetail(build.ctx_, *build.polyMesh_, *build.compactHeightField_, cfg.detailSampleDist,
            cfg.detailSampleMaxError, *build.polyMeshDetail_))
        {
            LOGERROR("Could not build detail mesh");
            return false;
        }
        
        // Set polygon flags.
        /// \todo Allow to define custom flags
        for (int i = 0; i < build.polyMesh_->npolys; ++i)
        {
            if (build.polyMesh_->areas[i] == RC_WALKABLE_AREA)
                build.polyMesh_->flags[i] = 0x1;
        }
        
        unsigned char* navData = 0;
        int navDataSize = 0;
        
        dtNavMeshCreateParams params;
        memset(&params, 0, sizeof params);
        params.verts = build.polyMesh_->verts;
        params.vertCount = build.polyMesh_->nverts;
        params.polys = build.polyMesh_->polys;
        params.polyAreas = build.polyMesh_->areas;
        params.polyFlags = build.polyMesh_->flags;
        params.polyCount = build.polyMesh_->npolys;
        params.nvp = build.polyMesh_->nvp;
        params.detailMeshes = build.polyMeshDetail_->meshes;
        params.detailVerts = build.polyMeshDetail_->verts;
        params.detailVertsCount = build.polyMeshDetail_->nverts;
        params.detailTris = build.polyMeshDetail_->tris;
        params.detailTriCount = build.polyMeshDetail_->ntris;
        params.walkableHeight = agentHeight_;
        params.walkableRadius = agentRadius_;
        params.walkableClimb = agentMaxClimb_;
        rcVcopy(params.bmin, build.polyMesh_->bmin);
        rcVcopy(params.bmax, build.polyMesh_->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;
        
        if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
        {
            LOGERROR("Could not build Detour navmesh");
            return false;
        }
        
        // Before creating the navmesh (which modifies the data) copy the data for serialization
        navigationDataAttr_.Resize(navDataSize);
        memcpy(&navigationDataAttr_[0], navData, navDataSize);
        
        return CreateNavMesh(navData, navDataSize);
    }
}

void NavigationMesh::FindPath(PODVector<Vector3>& dest, const Vector3& start, const Vector3& end, const Vector3& extents)
{
    PROFILE(FindPath);
    
    dest.Clear();
    
    if (!navMesh_ || !navMeshQuery_)
        return;
    
    // Navigation data is in local space. Transform path points from world to local
    const Matrix3x4& transform = node_->GetWorldTransform();
    Matrix3x4 inverseTransform = transform.Inverse();
    
    Vector3 localStart = inverseTransform * start;
    Vector3 localEnd = inverseTransform * end;
    
    dtPolyRef startRef;
    dtPolyRef endRef;
    navMeshQuery_->findNearestPoly(&localStart.x_, &extents.x_, queryFilter_, &startRef, 0);
    navMeshQuery_->findNearestPoly(&localEnd.x_, &extents.x_, queryFilter_, &endRef, 0);
    
    if (!startRef || !endRef)
        return;
    
    dtPolyRef polys[MAX_POLYS];
    dtPolyRef pathPolys[MAX_POLYS];
    Vector3 pathPoints[MAX_POLYS];
    unsigned char pathFlags[MAX_POLYS];
    int numPolys = 0;
    int numPathPoints = 0;
    
    navMeshQuery_->findPath(startRef, endRef, &localStart.x_, &localEnd.x_, queryFilter_, polys, &numPolys, MAX_POLYS);
    if (!numPolys)
        return;
    
    Vector3 actualLocalEnd = localEnd;
    
    // If full path was not found, clamp end point to the end polygon
    if (polys[numPolys - 1] != endRef)
        navMeshQuery_->closestPointOnPoly(polys[numPolys - 1], &localEnd.x_, &actualLocalEnd.x_);
    
    navMeshQuery_->findStraightPath(&localStart.x_, &actualLocalEnd.x_, polys, numPolys, &pathPoints[0].x_, pathFlags, pathPolys,
        &numPathPoints, MAX_POLYS);
    
    // Transform path result back to world space
    for (int i = 0; i < numPathPoints; ++i)
        dest.Push(transform * pathPoints[i]);
}

void NavigationMesh::SetNavigationDataAttr(const PODVector<unsigned char>& data)
{
    navigationDataAttr_ = data;
    if (!data.Size())
        return;
    
    /// \todo Would be preferable not to have to make a copy of the data, however Recast modifies it when creating the navmesh
    unsigned char* navData = (unsigned char*)dtAlloc(data.Size(), DT_ALLOC_PERM);
    memcpy(navData, &navigationDataAttr_[0], navigationDataAttr_.Size());
    
    CreateNavMesh(navData, data.Size());
}

void NavigationMesh::CollectGeometries(NavigationBuildData& build, Node* node, HashSet<Node*>& processedNodes, bool recursive)
{
    // Make sure nodes are not included twice
    if (processedNodes.Contains(node))
        return;
    processedNodes.Insert(node);
    
    /// \todo Prefer physics geometry if available
    PODVector<Drawable*> drawables;
    node->GetDerivedComponents<Drawable>(drawables);
    for (unsigned i = 0; i < drawables.Size(); ++i)
    {
        /// \todo Evaluate whether should handle other types. Now StaticModel & TerrainPatch are supported, others skipped
        Drawable* drawable = drawables[i];
        if (!drawable->IsEnabledEffective())
            continue;
        
        unsigned numGeometries = drawable->GetBatches().Size();
        unsigned lodLevel;
        if (drawable->GetType() == StaticModel::GetTypeStatic())
            lodLevel = static_cast<StaticModel*>(drawable)->GetOcclusionLodLevel();
        else if (drawable->GetType() == TerrainPatch::GetTypeStatic())
            lodLevel = 0;
        else
            continue;
        
        for (unsigned j = 0; j < numGeometries; ++j)
            AddGeometry(build, node, drawable->GetLodGeometry(j, lodLevel));
    }
    
    if (recursive)
    {
        const Vector<SharedPtr<Node> >& children = node->GetChildren();
        for(unsigned i = 0; i < children.Size(); ++i)
            CollectGeometries(build, children[i], processedNodes, recursive);
    }
}

void NavigationMesh::AddGeometry(NavigationBuildData& build, Node* node, Geometry* geometry)
{
    const unsigned char* vertexData;
    const unsigned char* indexData;
    unsigned vertexSize;
    unsigned indexSize;
    unsigned elementMask;
    
    // Transform geometry to local space of the navigation mesh node
    Matrix3x4 transform = node_->GetWorldTransform().Inverse() * node->GetWorldTransform();
    
    geometry->GetRawData(vertexData, vertexSize, indexData, indexSize, elementMask);
    if (!vertexData || !indexData || (elementMask & MASK_POSITION) == 0)
    {
        LOGERROR("Could not use geometry from node " + node->GetName() + ": vertex or index raw data null or positions missing");
        return;
    }
    
    unsigned srcIndexStart = geometry->GetIndexStart();
    unsigned srcIndexCount = geometry->GetIndexCount();
    unsigned srcVertexStart = geometry->GetVertexStart();
    unsigned srcVertexCount = geometry->GetVertexCount();
    
    if (!srcIndexCount)
    {
        LOGERROR("Could not use geometry from node " + node->GetName() + ": no indices");
        return;
    }
    
    unsigned destVertexStart = build.vertices_.Size();
    
    for (unsigned i = srcVertexStart; i < srcVertexStart + srcVertexCount; ++i)
    {
        Vector3 vertex = transform * *((const Vector3*)(&vertexData[i * vertexSize]));
        build.worldBoundingBox_.Merge(vertex);
        build.vertices_.Push(vertex);
    }
    
    // Copy remapped indices
    if (indexSize == sizeof(unsigned short))
    {
        const unsigned short* indices = ((const unsigned short*)indexData) + srcIndexStart;
        const unsigned short* indicesEnd = indices + srcIndexCount;
        
        while (indices < indicesEnd)
        {
            build.indices_.Push(*indices - srcVertexStart + destVertexStart);
            ++indices;
        }
    }
    else
    {
        const unsigned* indices = ((const unsigned*)indexData) + srcIndexStart;
        const unsigned* indicesEnd = indices + srcIndexCount;
        
        while (indices < indicesEnd)
        {
            build.indices_.Push(*indices - srcVertexStart + destVertexStart);
            ++indices;
        }
    }
}

bool NavigationMesh::CreateNavMesh(unsigned char* navData, unsigned navDataSize)
{
    ReleaseNavMesh();
    
    navMesh_ = dtAllocNavMesh();
    if (!navMesh_)
    {
        LOGERROR("Could not create Detour navmesh");
        dtFree(navData);
        return false;
    }
    
    dtStatus status;
    
    status = navMesh_->init(navData, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        LOGERROR("Could not init Detour navmesh");
        ReleaseNavMesh();
        dtFree(navData);
        return false;
    }
    
    navMeshQuery_ = dtAllocNavMeshQuery();
    if (!navMeshQuery_)
    {
        LOGERROR("Could not create Detour navmesh query");
        ReleaseNavMesh();
        return false;
    }
    
    status = navMeshQuery_->init(navMesh_, 2048);
    if (dtStatusFailed(status))
    {
        LOGERROR("Could not init navmesh query");
        ReleaseNavMesh();
        return false;
    }
    
    LOGDEBUG("Created Detour navmesh, data size " + String(navDataSize) + " bytes");
    return true;
}

void NavigationMesh::ReleaseNavMesh()
{
    dtFreeNavMesh(navMesh_);
    navMesh_ = 0;
    
    dtFreeNavMeshQuery(navMeshQuery_);
    navMeshQuery_ = 0;
}

}
