#include "makenavmesh.hpp"
#include "chunkytrimesh.hpp"
#include "debug.hpp"
#include "dtstatus.hpp"
#include "exceptions.hpp"
#include "recastmesh.hpp"
#include "settings.hpp"
#include "settingsutils.hpp"
#include "sharednavmesh.hpp"
#include "settingsutils.hpp"
#include "flags.hpp"
#include "navmeshtilescache.hpp"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>
#include <RecastAlloc.h>

#include <algorithm>
#include <iomanip>
#include <limits>

namespace
{
    using namespace DetourNavigator;

    static const int doNotTransferOwnership = 0;

    void initPolyMeshDetail(rcPolyMeshDetail& value)
    {
        value.meshes = nullptr;
        value.verts = nullptr;
        value.tris = nullptr;
    }

    struct PolyMeshDetailStackDeleter
    {
        void operator ()(rcPolyMeshDetail* value) const
        {
            rcFree(value->meshes);
            rcFree(value->verts);
            rcFree(value->tris);
        }
    };

    using PolyMeshDetailStackPtr = std::unique_ptr<rcPolyMeshDetail, PolyMeshDetailStackDeleter>;

    osg::Vec3f makeOsgVec3f(const btVector3& value)
    {
        return osg::Vec3f(value.x(), value.y(), value.z());
    }

    struct WaterBounds
    {
        osg::Vec3f mMin;
        osg::Vec3f mMax;
    };

    WaterBounds getWaterBounds(const RecastMesh::Water& water, const Settings& settings,
        const osg::Vec3f& agentHalfExtents)
    {
        if (water.mCellSize == std::numeric_limits<int>::max())
        {
            const auto transform = getSwimLevelTransform(settings, water.mTransform, agentHalfExtents.z());
            const auto min = toNavMeshCoordinates(settings, makeOsgVec3f(transform(btVector3(-1, -1, 0))));
            const auto max = toNavMeshCoordinates(settings, makeOsgVec3f(transform(btVector3(1, 1, 0))));
            return WaterBounds {
                osg::Vec3f(-std::numeric_limits<float>::max(), min.y(), -std::numeric_limits<float>::max()),
                osg::Vec3f(std::numeric_limits<float>::max(), max.y(), std::numeric_limits<float>::max())
            };
        }
        else
        {
            const auto transform = getSwimLevelTransform(settings, water.mTransform, agentHalfExtents.z());
            const auto halfCellSize = water.mCellSize / 2.0f;
            return WaterBounds {
                toNavMeshCoordinates(settings, makeOsgVec3f(transform(btVector3(-halfCellSize, -halfCellSize, 0)))),
                toNavMeshCoordinates(settings, makeOsgVec3f(transform(btVector3(halfCellSize, halfCellSize, 0))))
            };
        }
    }

    std::vector<float> getOffMeshVerts(const std::vector<OffMeshConnection>& connections)
    {
        std::vector<float> result;

        result.reserve(connections.size() * 6);

        const auto add = [&] (const osg::Vec3f& v)
        {
            result.push_back(v.x());
            result.push_back(v.y());
            result.push_back(v.z());
        };

        for (const auto& v : connections)
        {
            add(v.mStart);
            add(v.mEnd);
        }

        return result;
    }

    void createHeightfield(rcContext& context, rcHeightfield& solid, int width, int height, const float* bmin,
        const float* bmax, const float cs, const float ch)
    {
        const auto result = rcCreateHeightfield(&context, solid, width, height, bmin, bmax, cs, ch);

        if (!result)
            throw NavigatorException("Failed to create heightfield for navmesh");
    }

    void buildCompactHeightfield(rcContext& context, const int walkableHeight, const int walkableClimb,
                                 rcHeightfield& solid, rcCompactHeightfield& compact)
    {
        const auto result = rcBuildCompactHeightfield(&context, walkableHeight,
            walkableClimb, solid, compact);

        if (!result)
            throw NavigatorException("Failed to build compact heightfield for navmesh");
    }

    void erodeWalkableArea(rcContext& context, int walkableRadius, rcCompactHeightfield& compact)
    {
        const auto result = rcErodeWalkableArea(&context, walkableRadius, compact);

        if (!result)
            throw NavigatorException("Failed to erode walkable area for navmesh");
    }

    void buildDistanceField(rcContext& context, rcCompactHeightfield& compact)
    {
        const auto result = rcBuildDistanceField(&context, compact);

        if (!result)
            throw NavigatorException("Failed to build distance field for navmesh");
    }

    void buildRegions(rcContext& context, rcCompactHeightfield& compact, const int borderSize,
        const int minRegionArea, const int mergeRegionArea)
    {
        const auto result = rcBuildRegions(&context, compact, borderSize, minRegionArea, mergeRegionArea);

        if (!result)
            throw NavigatorException("Failed to build distance field for navmesh");
    }

    void buildContours(rcContext& context, rcCompactHeightfield& compact, const float maxError, const int maxEdgeLen,
        rcContourSet& contourSet, const int buildFlags = RC_CONTOUR_TESS_WALL_EDGES)
    {
        const auto result = rcBuildContours(&context, compact, maxError, maxEdgeLen, contourSet, buildFlags);

        if (!result)
            throw NavigatorException("Failed to build contours for navmesh");
    }

    void buildPolyMesh(rcContext& context, rcContourSet& contourSet, const int maxVertsPerPoly, rcPolyMesh& polyMesh)
    {
        const auto result = rcBuildPolyMesh(&context, contourSet, maxVertsPerPoly, polyMesh);

        if (!result)
            throw NavigatorException("Failed to build poly mesh for navmesh");
    }

    void buildPolyMeshDetail(rcContext& context, const rcPolyMesh& polyMesh, const rcCompactHeightfield& compact,
        const float sampleDist, const float sampleMaxError, rcPolyMeshDetail& polyMeshDetail)
    {
        const auto result = rcBuildPolyMeshDetail(&context, polyMesh, compact, sampleDist, sampleMaxError,
                                                  polyMeshDetail);

        if (!result)
            throw NavigatorException("Failed to build detail poly mesh for navmesh");
    }

    NavMeshData makeNavMeshTileData(const osg::Vec3f& agentHalfExtents, const RecastMesh& recastMesh,
        const std::vector<OffMeshConnection>& offMeshConnections, const int tileX, const int tileY,
        const osg::Vec3f& boundsMin, const osg::Vec3f& boundsMax, const Settings& settings)
    {
        rcContext context;
        rcConfig config;

        config.cs = settings.mCellSize;
        config.ch = settings.mCellHeight;
        config.walkableSlopeAngle = settings.mMaxSlope;
        config.walkableHeight = static_cast<int>(std::ceil(getHeight(settings, agentHalfExtents) / config.ch));
        config.walkableClimb = static_cast<int>(std::floor(getMaxClimb(settings) / config.ch));
        config.walkableRadius = static_cast<int>(std::ceil(getRadius(settings, agentHalfExtents) / config.cs));
        config.maxEdgeLen = static_cast<int>(std::round(settings.mMaxEdgeLen / config.cs));
        config.maxSimplificationError = settings.mMaxSimplificationError;
        config.minRegionArea = settings.mRegionMinSize * settings.mRegionMinSize;
        config.mergeRegionArea = settings.mRegionMergeSize * settings.mRegionMergeSize;
        config.maxVertsPerPoly = settings.mMaxVertsPerPoly;
        config.detailSampleDist = settings.mDetailSampleDist < 0.9f ? 0 : config.cs * settings.mDetailSampleDist;
        config.detailSampleMaxError = config.ch * settings.mDetailSampleMaxError;
        config.borderSize = settings.mBorderSize;
        config.width = settings.mTileSize + config.borderSize * 2;
        config.height = settings.mTileSize + config.borderSize * 2;
        rcVcopy(config.bmin, boundsMin.ptr());
        rcVcopy(config.bmax, boundsMax.ptr());
        config.bmin[0] -= getBorderSize(settings);
        config.bmin[2] -= getBorderSize(settings);
        config.bmax[0] += getBorderSize(settings);
        config.bmax[2] += getBorderSize(settings);

        rcHeightfield solid;
        createHeightfield(context, solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch);

        {
            const auto& chunkyMesh = recastMesh.getChunkyTriMesh();
            std::vector<unsigned char> areas(chunkyMesh.getMaxTrisPerChunk(), AreaType_null);
            const osg::Vec2f tileBoundsMin(config.bmin[0], config.bmin[2]);
            const osg::Vec2f tileBoundsMax(config.bmax[0], config.bmax[2]);
            std::vector<std::size_t> cids;
            chunkyMesh.getChunksOverlappingRect(Rect {tileBoundsMin, tileBoundsMax}, std::back_inserter(cids));

            if (cids.empty())
                return NavMeshData();

            for (const auto cid : cids)
            {
                const auto chunk = chunkyMesh.getChunk(cid);

                std::fill(
                    areas.begin(),
                    std::min(areas.begin() + static_cast<std::ptrdiff_t>(chunk.mSize),
                    areas.end()),
                    AreaType_null
                );

                rcMarkWalkableTriangles(
                    &context,
                    config.walkableSlopeAngle,
                    recastMesh.getVertices().data(),
                    static_cast<int>(recastMesh.getVerticesCount()),
                    chunk.mIndices,
                    static_cast<int>(chunk.mSize),
                    areas.data()
                );

                for (std::size_t i = 0; i < chunk.mSize; ++i)
                    areas[i] = chunk.mAreaTypes[i];

                rcClearUnwalkableTriangles(
                    &context,
                    config.walkableSlopeAngle,
                    recastMesh.getVertices().data(),
                    static_cast<int>(recastMesh.getVerticesCount()),
                    chunk.mIndices,
                    static_cast<int>(chunk.mSize),
                    areas.data()
                );

                const auto trianglesRasterized = rcRasterizeTriangles(
                    &context,
                    recastMesh.getVertices().data(),
                    static_cast<int>(recastMesh.getVerticesCount()),
                    chunk.mIndices,
                    areas.data(),
                    static_cast<int>(chunk.mSize),
                    solid,
                    config.walkableClimb
                );

                if (!trianglesRasterized)
                    throw NavigatorException("Failed to create rasterize triangles from recast mesh for navmesh");
            }
        }

        {
            const std::array<unsigned char, 2> areas {{AreaType_water, AreaType_water}};

            for (const auto& water : recastMesh.getWater())
            {
                const auto bounds = getWaterBounds(water, settings, agentHalfExtents);

                const osg::Vec2f tileBoundsMin(
                    std::min(config.bmax[0], std::max(config.bmin[0], bounds.mMin.x())),
                    std::min(config.bmax[2], std::max(config.bmin[2], bounds.mMin.z()))
                );
                const osg::Vec2f tileBoundsMax(
                    std::min(config.bmax[0], std::max(config.bmin[0], bounds.mMax.x())),
                    std::min(config.bmax[2], std::max(config.bmin[2], bounds.mMax.z()))
                );

                if (tileBoundsMax == tileBoundsMin)
                    continue;

                const std::array<osg::Vec3f, 4> vertices {{
                    osg::Vec3f(tileBoundsMin.x(), bounds.mMin.y(), tileBoundsMin.y()),
                    osg::Vec3f(tileBoundsMin.x(), bounds.mMin.y(), tileBoundsMax.y()),
                    osg::Vec3f(tileBoundsMax.x(), bounds.mMin.y(), tileBoundsMax.y()),
                    osg::Vec3f(tileBoundsMax.x(), bounds.mMin.y(), tileBoundsMin.y()),
                }};

                std::array<float, 4 * 3> convertedVertices;
                auto convertedVerticesIt = convertedVertices.begin();

                for (const auto& vertex : vertices)
                    convertedVerticesIt = std::copy(vertex.ptr(), vertex.ptr() + 3, convertedVerticesIt);

                const std::array<int, 6> indices {{
                    0, 1, 2,
                    0, 2, 3,
                }};

                const auto trianglesRasterized = rcRasterizeTriangles(
                    &context,
                    convertedVertices.data(),
                    static_cast<int>(convertedVertices.size() / 3),
                    indices.data(),
                    areas.data(),
                    static_cast<int>(areas.size()),
                    solid,
                    config.walkableClimb
                );

                if (!trianglesRasterized)
                    throw NavigatorException("Failed to create rasterize water triangles for navmesh");
            }
        }

        rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, solid);
        rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, solid);
        rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, solid);

        rcPolyMesh polyMesh;
        rcPolyMeshDetail polyMeshDetail;
        initPolyMeshDetail(polyMeshDetail);
        const PolyMeshDetailStackPtr polyMeshDetailPtr(&polyMeshDetail);
        {
            rcCompactHeightfield compact;
            buildCompactHeightfield(context, config.walkableHeight, config.walkableClimb, solid, compact);

            erodeWalkableArea(context, config.walkableRadius, compact);
            buildDistanceField(context, compact);
            buildRegions(context, compact, config.borderSize, config.minRegionArea, config.mergeRegionArea);

            rcContourSet contourSet;
            buildContours(context, compact, config.maxSimplificationError, config.maxEdgeLen, contourSet);

            if (contourSet.nconts == 0)
                return NavMeshData();

            buildPolyMesh(context, contourSet, config.maxVertsPerPoly, polyMesh);

            buildPolyMeshDetail(context, polyMesh, compact, config.detailSampleDist, config.detailSampleMaxError,
                                polyMeshDetail);
        }

        for (int i = 0; i < polyMesh.npolys; ++i)
        {
            if (polyMesh.areas[i] == AreaType_ground)
                polyMesh.flags[i] = Flag_walk;
            else if (polyMesh.areas[i] == AreaType_water)
                polyMesh.flags[i] = Flag_swim;
        }

        const auto offMeshConVerts = getOffMeshVerts(offMeshConnections);
        const std::vector<float> offMeshConRad(offMeshConnections.size(), getRadius(settings, agentHalfExtents));
        const std::vector<unsigned char> offMeshConDir(offMeshConnections.size(), DT_OFFMESH_CON_BIDIR);
        const std::vector<unsigned char> offMeshConAreas(offMeshConnections.size(), AreaType_ground);
        const std::vector<unsigned short> offMeshConFlags(offMeshConnections.size(), Flag_openDoor);

        dtNavMeshCreateParams params;
        params.verts = polyMesh.verts;
        params.vertCount = polyMesh.nverts;
        params.polys = polyMesh.polys;
        params.polyAreas = polyMesh.areas;
        params.polyFlags = polyMesh.flags;
        params.polyCount = polyMesh.npolys;
        params.nvp = polyMesh.nvp;
        params.detailMeshes = polyMeshDetail.meshes;
        params.detailVerts = polyMeshDetail.verts;
        params.detailVertsCount = polyMeshDetail.nverts;
        params.detailTris = polyMeshDetail.tris;
        params.detailTriCount = polyMeshDetail.ntris;
        params.offMeshConVerts = offMeshConVerts.data();
        params.offMeshConRad = offMeshConRad.data();
        params.offMeshConDir = offMeshConDir.data();
        params.offMeshConAreas = offMeshConAreas.data();
        params.offMeshConFlags = offMeshConFlags.data();
        params.offMeshConUserID = nullptr;
        params.offMeshConCount = static_cast<int>(offMeshConnections.size());
        params.walkableHeight = getHeight(settings, agentHalfExtents);
        params.walkableRadius = getRadius(settings, agentHalfExtents);
        params.walkableClimb = getMaxClimb(settings);
        rcVcopy(params.bmin, polyMesh.bmin);
        rcVcopy(params.bmax, polyMesh.bmax);
        params.cs = config.cs;
        params.ch = config.ch;
        params.buildBvTree = true;
        params.userId = 0;
        params.tileX = tileX;
        params.tileY = tileY;
        params.tileLayer = 0;

        unsigned char* navMeshData;
        int navMeshDataSize;
        const auto navMeshDataCreated = dtCreateNavMeshData(&params, &navMeshData, &navMeshDataSize);

        if (!navMeshDataCreated)
            throw NavigatorException("Failed to create navmesh tile data");

        return NavMeshData(navMeshData, navMeshDataSize);
    }

    UpdateNavMeshStatus makeUpdateNavMeshStatus(bool removed, bool add)
    {
        if (removed && add)
            return UpdateNavMeshStatus::replaced;
        else if (removed)
            return UpdateNavMeshStatus::removed;
        else if (add)
            return UpdateNavMeshStatus::add;
        else
            return UpdateNavMeshStatus::ignore;
    }
}

namespace DetourNavigator
{
    NavMeshPtr makeEmptyNavMesh(const Settings& settings)
    {
        // Max tiles and max polys affect how the tile IDs are caculated.
        // There are 22 bits available for identifying a tile and a polygon.
        const auto tileBits = 10;
        const auto polyBits = 22 - tileBits;
        const auto maxTiles = 1 << tileBits;
        const auto maxPolysPerTile = 1 << polyBits;

        dtNavMeshParams params;
        std::fill_n(params.orig, 3, 0.0f);
        params.tileWidth = settings.mTileSize * settings.mCellSize;
        params.tileHeight = settings.mTileSize * settings.mCellSize;
        params.maxTiles = maxTiles;
        params.maxPolys = maxPolysPerTile;

        NavMeshPtr navMesh(dtAllocNavMesh(), &dtFreeNavMesh);
        const auto status = navMesh->init(&params);

        if (!dtStatusSucceed(status))
            throw NavigatorException("Failed to init navmesh");

        return navMesh;
    }

    UpdateNavMeshStatus updateNavMesh(const osg::Vec3f& agentHalfExtents, const RecastMesh* recastMesh,
        const TilePosition& changedTile, const TilePosition& playerTile,
        const std::vector<OffMeshConnection>& offMeshConnections, const Settings& settings,
        const SharedNavMeshCacheItem& navMeshCacheItem, NavMeshTilesCache& navMeshTilesCache)
    {
        log("update NavMesh with mutiple tiles:",
            " agentHeight=", std::setprecision(std::numeric_limits<float>::max_exponent10),
            getHeight(settings, agentHalfExtents),
            " agentMaxClimb=", std::setprecision(std::numeric_limits<float>::max_exponent10),
            getMaxClimb(settings),
            " agentRadius=", std::setprecision(std::numeric_limits<float>::max_exponent10),
            getRadius(settings, agentHalfExtents),
            " changedTile=", changedTile,
            " playerTile=", playerTile,
            " changedTileDistance=", getDistance(changedTile, playerTile));

        const auto params = *navMeshCacheItem.lockConst()->getValue().getParams();
        const osg::Vec3f origin(params.orig[0], params.orig[1], params.orig[2]);

        const auto x = changedTile.x();
        const auto y = changedTile.y();

        const auto removeTile = [&] {
            const auto locked = navMeshCacheItem.lock();
            auto& navMesh = locked->getValue();
            const auto tileRef = navMesh.getTileRefAt(x, y, 0);
            const auto removed = dtStatusSucceed(navMesh.removeTile(tileRef, nullptr, nullptr));
            if (removed)
                locked->removeUsedTile(changedTile);
            return makeUpdateNavMeshStatus(removed, false);
        };

        if (!recastMesh)
        {
            log("ignore add tile: recastMesh is null");
            return removeTile();
        }

        auto boundsMin = recastMesh->getBoundsMin();
        auto boundsMax = recastMesh->getBoundsMax();

        for (const auto& water : recastMesh->getWater())
        {
            const auto bounds = getWaterBounds(water, settings, agentHalfExtents);
            boundsMin.y() = std::min(boundsMin.y(), bounds.mMin.y());
            boundsMax.y() = std::max(boundsMax.y(), bounds.mMax.y());
        }

        if (boundsMin == boundsMax)
        {
            log("ignore add tile: recastMesh is empty");
            return removeTile();
        }

        if (!shouldAddTile(changedTile, playerTile, params.maxTiles))
        {
            log("ignore add tile: too far from player");
            return removeTile();
        }

        auto cachedNavMeshData = navMeshTilesCache.get(agentHalfExtents, changedTile, *recastMesh, offMeshConnections);

        if (!cachedNavMeshData)
        {
            const auto tileBounds = makeTileBounds(settings, changedTile);
            const osg::Vec3f tileBorderMin(tileBounds.mMin.x(), boundsMin.y() - 1, tileBounds.mMin.y());
            const osg::Vec3f tileBorderMax(tileBounds.mMax.x(), boundsMax.y() + 1, tileBounds.mMax.y());

            auto navMeshData = makeNavMeshTileData(agentHalfExtents, *recastMesh, offMeshConnections, x, y,
                tileBorderMin, tileBorderMax, settings);

            if (!navMeshData.mValue)
            {
                log("ignore add tile: NavMeshData is null");
                return removeTile();
            }

            try
            {
                cachedNavMeshData = navMeshTilesCache.set(agentHalfExtents, changedTile, *recastMesh,
                                                          offMeshConnections, std::move(navMeshData));
            }
            catch (const InvalidArgument&)
            {
                cachedNavMeshData = navMeshTilesCache.get(agentHalfExtents, changedTile, *recastMesh,
                                                          offMeshConnections);
            }

            if (!cachedNavMeshData)
            {
                log("cache overflow");

                const auto locked = navMeshCacheItem.lock();
                auto& navMesh = locked->getValue();
                const auto tileRef = navMesh.getTileRefAt(x, y, 0);
                const auto removed = dtStatusSucceed(navMesh.removeTile(tileRef, nullptr, nullptr));
                const auto addStatus = navMesh.addTile(navMeshData.mValue.get(), navMeshData.mSize,
                                                       doNotTransferOwnership, 0, 0);

                if (dtStatusSucceed(addStatus))
                {
                    locked->setUsedTile(changedTile, std::move(navMeshData));
                    return makeUpdateNavMeshStatus(removed, true);
                }
                else
                {
                    if (removed)
                        locked->removeUsedTile(changedTile);
                    log("failed to add tile with status=", WriteDtStatus {addStatus});
                    return makeUpdateNavMeshStatus(removed, false);
                }
            }
        }

        const auto locked = navMeshCacheItem.lock();
        auto& navMesh = locked->getValue();
        const auto tileRef = navMesh.getTileRefAt(x, y, 0);
        const auto removed = dtStatusSucceed(navMesh.removeTile(tileRef, nullptr, nullptr));
        const auto addStatus = navMesh.addTile(cachedNavMeshData.get().mValue, cachedNavMeshData.get().mSize,
                                               doNotTransferOwnership, 0, 0);

        if (dtStatusSucceed(addStatus))
        {
            locked->setUsedTile(changedTile, std::move(cachedNavMeshData));
            return makeUpdateNavMeshStatus(removed, true);
        }
        else
        {
            if (removed)
                locked->removeUsedTile(changedTile);
            log("failed to add tile with status=", WriteDtStatus {addStatus});
            return makeUpdateNavMeshStatus(removed, false);
        }
    }
}
