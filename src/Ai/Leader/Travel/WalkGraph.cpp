/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WalkGraph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "Common.h"
#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

namespace Leader::Travel::WalkGraph
{
    namespace
    {
        constexpr std::array<uint32, 4> GRAPH_MAPS = {0, 1, 530, 571};  // the continents (long walks)
        constexpr uint16 NO_REGION = 0xFFFF;
        // the route search's filter (LongRoute) for a walker that does not take steep ground
        constexpr uint16 WALK_INCLUDE = uint16(NAV_GROUND | NAV_WATER);
        constexpr uint16 WALK_EXCLUDE = uint16(NAV_MAGMA | NAV_SLIME);
        constexpr uint32 NO_NODE = std::numeric_limits<uint32>::max();

        struct Node
        {
            G3D::Vector3 pos;
            uint32 cluster = 0;
            uint16 region = NO_REGION;
            uint32 firstEdge = 0;
            uint32 edgeCount = 0;
        };
        struct Edge
        {
            uint32 to = 0;
            float yd = 0.0f;
        };
        struct TileRegions
        {
            uint32 cluster = 0;
            std::vector<uint16> region;  // per polygon of the tile, its cluster region (NO_REGION: not walkable)
        };
        struct MapGraph
        {
            uint32 mapId = 0;
            std::vector<Node> nodes;
            std::vector<Edge> edges;
            std::unordered_map<uint64, TileRegions> tiles;                 // TileKey(x, y, layer)
            std::unordered_map<uint64, uint32> tileCluster;                // TileKey(x, y, 0): the tile's cluster
            std::unordered_map<uint64, std::vector<uint32>> regionNodes;   // RegionKey(cluster, region)
            std::unordered_map<uint32, std::vector<uint32>> clusterNodes;  // cluster: its nodes
        };

        std::array<std::atomic<MapGraph const*>, GRAPH_MAPS.size()> published{};
        std::array<std::unique_ptr<MapGraph>, GRAPH_MAPS.size()> owned;  // freed at exit, after the map threads
        std::thread builder;
        std::atomic<bool> stopBuild{false};

        uint64 TileKey(int x, int y, int layer)
        {
            return (uint64(uint32(x) & 0xFFFF) << 32) | (uint64(uint32(y) & 0xFFFF) << 16) |
                   uint64(uint32(layer) & 0xFFFF);
        }
        uint64 RegionKey(uint32 cluster, uint16 region) { return (uint64(cluster) << 16) | region; }
        // a polygon by its tile's grid place and index: the same in the builder's navmesh and the live one
        uint64 PolyKey(dtMeshHeader const* header, uint32 index)
        {
            return (uint64(uint32(header->x) & 0x3FF) << 36) | (uint64(uint32(header->y) & 0x3FF) << 26) |
                   (uint64(uint32(header->layer) & 0x3F) << 20) | uint64(index & 0xFFFFF);
        }
        // a polygon's entrance toward (or from) another cluster: 46 bits of polygon, 12 of cluster (a continent has
        // fewer than 4096 grids)
        uint64 ExitKey(uint64 polyKey, uint32 cluster) { return (polyKey << 12) | (cluster & 0xFFF); }
        // Detour (y, z, x) -> world
        G3D::Vector3 ToWorld(float const* v) { return G3D::Vector3(v[2], v[0], v[1]); }

        bool Walkable(dtPoly const* poly)
        {
            return (poly->flags & WALK_INCLUDE) != 0 && (poly->flags & WALK_EXCLUDE) == 0;
        }

        int SlotOf(uint32 mapId)
        {
            for (size_t i = 0; i < GRAPH_MAPS.size(); ++i)
                if (GRAPH_MAPS[i] == mapId)
                    return int(i);
            return -1;
        }

        // where a walk over `link` (from `poly` of `tile`) enters the next polygon: the portal's midpoint (Detour's
        // own A* metric), an off-mesh link's end vertex
        G3D::Vector3 PortalPos(dtNavMesh const* mesh, dtMeshTile const* tile, dtPoly const* poly, dtLink const& link)
        {
            if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            {
                uint8 const v = link.edge < poly->vertCount ? link.edge : uint8(poly->vertCount - 1);
                return ToWorld(&tile->verts[poly->verts[v] * 3]);
            }
            dtMeshTile const* nextTile = nullptr;
            dtPoly const* next = nullptr;
            mesh->getTileAndPolyByRefUnsafe(link.ref, &nextTile, &next);
            if (next->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
            {
                // onto an off-mesh link: its vertex nearer to where this polygon lies
                float const* a = &nextTile->verts[next->verts[0] * 3];
                float const* b = &nextTile->verts[next->verts[1] * 3];
                float const* own = &tile->verts[poly->verts[0] * 3];
                return ToWorld(dtVdistSqr(own, a) <= dtVdistSqr(own, b) ? a : b);
            }
            if (link.edge >= poly->vertCount)
                return ToWorld(&tile->verts[poly->verts[0] * 3]);
            float const* va = &tile->verts[poly->verts[link.edge] * 3];
            float const* vb = &tile->verts[poly->verts[(link.edge + 1) % poly->vertCount] * 3];
            float s = 0.0f;
            float t = 1.0f;
            if (link.side != 0xFF && (link.bmin != 0 || link.bmax != 255))  // a tile edge's part shared with the next
            {
                s = float(link.bmin) / 255.0f;
                t = float(link.bmax) / 255.0f;
            }
            float mid[3];
            float const u = 0.5f * (s + t);
            dtVlerp(mid, va, vb, u);
            return ToWorld(mid);
        }

        class Builder
        {
        public:
            Builder(uint32 mapId, std::string const& dataPath) : _mapId(mapId), _dataPath(dataPath) {}
            ~Builder()
            {
                if (_mesh)
                    dtFreeNavMesh(_mesh);
            }

            std::unique_ptr<MapGraph> Build();

        private:
            struct Cluster
            {
                int a = 0;  // the mmtile name's grid numbers
                int b = 0;
                std::vector<dtTileRef> tiles;
                bool loaded = false;
            };
            struct Crossing
            {
                uint32 from = 0;      // the node in the cluster left
                uint64 toPoly = 0;    // the polygon entered
                uint32 fromCluster = 0;
            };

            bool Load(uint32 cluster);
            void Unload(uint32 cluster);
            void Process(uint32 cluster);
            int32 OwnerOf(dtPolyRef ref) const { return _tileOwner[_mesh->decodePolyIdTile(ref)]; }

            uint32 _mapId;
            std::string _dataPath;
            dtNavMesh* _mesh = nullptr;
            std::vector<Cluster> _clusters;
            std::vector<int32> _tileOwner;  // per Detour tile index: its cluster, -1 none
            std::vector<int32> _tileBase;   // per Detour tile index: its first local polygon in the cluster processed
            std::unique_ptr<MapGraph> _graph;
            std::vector<Crossing> _crossings;
            std::vector<std::tuple<uint32, uint32, float>> _walks;     // (from, to, yd) inside a cluster
            std::unordered_map<uint64, uint32> _exitNode;             // (poly key, other cluster) -> node
            uint64 _polys = 0;
            uint64 _regions = 0;
            uint64 _expanded = 0;
        };

        bool Builder::Load(uint32 index)
        {
            Cluster& cluster = _clusters[index];
            if (cluster.loaded)
                return true;
            cluster.loaded = true;
            std::string const name = Acore::StringFormat("{}mmaps/{:03}{:02}{:02}.mmtile", _dataPath, _mapId,
                                                         cluster.a, cluster.b);
            FILE* file = fopen(name.c_str(), "rb");
            if (!file)
                return false;
            MmapTileHeader header;
            if (fread(&header, sizeof(MmapTileHeader), 1, file) != 1 || header.mmapMagic != MMAP_MAGIC ||
                header.mmapVersion != MMAP_VERSION)
            {
                fclose(file);
                return false;
            }
            uint32 remaining = header.size;
            for (uint16 i = 0; i < header.tileCount; ++i)
            {
                uint32 dataSize = 0;
                if (remaining < sizeof(dataSize) || fread(&dataSize, sizeof(dataSize), 1, file) != 1 || !dataSize ||
                    dataSize > remaining - sizeof(dataSize))
                    break;
                remaining -= sizeof(dataSize) + dataSize;
                unsigned char* data = static_cast<unsigned char*>(dtAlloc(dataSize, DT_ALLOC_PERM));
                if (!data)
                    break;
                if (fread(data, dataSize, 1, file) != 1)
                {
                    dtFree(data);
                    break;
                }
                dtTileRef ref = 0;
                if (dtStatusFailed(_mesh->addTile(data, dataSize, DT_TILE_FREE_DATA, 0, &ref)))
                {
                    dtFree(data);
                    continue;
                }
                cluster.tiles.push_back(ref);
                _tileOwner[_mesh->decodePolyIdTile(ref)] = int32(index);
            }
            fclose(file);
            return true;
        }

        void Builder::Unload(uint32 index)
        {
            Cluster& cluster = _clusters[index];
            for (dtTileRef ref : cluster.tiles)
            {
                _tileOwner[_mesh->decodePolyIdTile(ref)] = -1;
                _mesh->removeTile(ref, nullptr, nullptr);
            }
            cluster.tiles.clear();
            cluster.loaded = false;
        }

        void Builder::Process(uint32 index)
        {
            Cluster const& cluster = _clusters[index];
            std::vector<dtMeshTile const*> tiles;
            uint32 total = 0;
            for (dtTileRef ref : cluster.tiles)
                if (dtMeshTile const* tile = _mesh->getTileByRef(ref); tile && tile->header)
                {
                    _tileBase[_mesh->decodePolyIdTile(ref)] = int32(total);
                    total += uint32(tile->header->polyCount);
                    tiles.push_back(tile);
                }
            _polys += total;
            // local polygon index -> its tile and polygon
            std::vector<std::pair<dtMeshTile const*, dtPoly const*>> polys(total);
            std::vector<uint8> walkable(total, 0);
            for (dtMeshTile const* tile : tiles)
            {
                uint32 const base = uint32(_tileBase[_mesh->decodePolyIdTile(_mesh->getTileRef(tile))]);
                for (int i = 0; i < tile->header->polyCount; ++i)
                {
                    polys[base + i] = {tile, &tile->polys[i]};
                    walkable[base + i] = Walkable(&tile->polys[i]) ? 1 : 0;
                }
            }
            auto const local = [&](dtPolyRef ref) -> int64
            {
                unsigned int const it = _mesh->decodePolyIdTile(ref);
                if (_tileOwner[it] != int32(index) || _tileBase[it] < 0)
                    return -1;
                return int64(_tileBase[it]) + int64(_mesh->decodePolyIdPoly(ref));
            };
            auto const forLinks = [&](uint32 at, auto&& visit)
            {
                auto const [tile, poly] = polys[at];
                for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                    visit(tile->links[l]);
            };

            // the cluster's regions: its walkable polygons joined by links inside it (either way)
            std::vector<uint32> parent(total);
            for (uint32 i = 0; i < total; ++i)
                parent[i] = i;
            auto const rootOf = [&](uint32 x)
            {
                while (parent[x] != x)
                {
                    parent[x] = parent[parent[x]];
                    x = parent[x];
                }
                return x;
            };
            for (uint32 i = 0; i < total; ++i)
            {
                if (!walkable[i])
                    continue;
                forLinks(i,
                         [&](dtLink const& link)
                         {
                             int64 const j = local(link.ref);
                             if (j >= 0 && walkable[size_t(j)])
                                 parent[rootOf(i)] = rootOf(uint32(j));
                         });
            }
            std::vector<uint16> region(total, NO_REGION);
            std::unordered_map<uint32, uint16> rootRegion;
            for (uint32 i = 0; i < total; ++i)
            {
                if (!walkable[i])
                    continue;
                uint16 const next = uint16(std::min<size_t>(rootRegion.size(), 0xFFFE));
                region[i] = rootRegion.emplace(rootOf(i), next).first->second;
            }
            _regions += rootRegion.size();
            for (dtMeshTile const* tile : tiles)
            {
                uint32 const base = uint32(_tileBase[_mesh->decodePolyIdTile(_mesh->getTileRef(tile))]);
                TileRegions& stored = _graph->tiles[TileKey(tile->header->x, tile->header->y, tile->header->layer)];
                stored.cluster = index;
                stored.region.assign(region.begin() + base, region.begin() + base + tile->header->polyCount);
                _graph->tileCluster[TileKey(tile->header->x, tile->header->y, 0)] = index;
            }

            // the exits: a walkable polygon's link into a walkable polygon of another cluster
            struct Exit
            {
                uint32 poly = 0;
                dtPolyRef to = 0;
                uint32 toCluster = 0;
                G3D::Vector3 pos;
            };
            std::vector<Exit> exits;
            for (uint32 i = 0; i < total; ++i)
            {
                if (!walkable[i])
                    continue;
                dtMeshTile const* tile = polys[i].first;
                dtPoly const* poly = polys[i].second;
                forLinks(i,
                         [&](dtLink const& link)
                         {
                             int32 const owner = OwnerOf(link.ref);
                             if (owner < 0 || owner == int32(index))
                                 return;
                             dtMeshTile const* nextTile = nullptr;
                             dtPoly const* next = nullptr;
                             _mesh->getTileAndPolyByRefUnsafe(link.ref, &nextTile, &next);
                             if (!Walkable(next))
                                 return;
                             // an off-mesh link's entrance is its vertex on this side (the link's edge is the other)
                             G3D::Vector3 const pos =
                                 poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION
                                     ? ToWorld(&tile->verts[poly->verts[link.edge == 0 ? 1 : 0] * 3])
                                     : PortalPos(_mesh, tile, poly, link);
                             exits.push_back({i, link.ref, uint32(owner), pos});
                         });
            }
            // one entrance per run of exit polygons toward the same cluster joined by links among them
            std::vector<uint32> group(exits.size());
            for (uint32 i = 0; i < group.size(); ++i)
                group[i] = i;
            auto const findGroup = [&](uint32 x)
            {
                while (group[x] != x)
                {
                    group[x] = group[group[x]];
                    x = group[x];
                }
                return x;
            };
            std::unordered_map<uint64, std::vector<uint32>> exitsAt;  // (local poly, to cluster) -> exits
            for (uint32 e = 0; e < exits.size(); ++e)
                exitsAt[(uint64(exits[e].poly) << 16) | exits[e].toCluster].push_back(e);
            for (auto const& [key, list] : exitsAt)
            {
                for (size_t k = 1; k < list.size(); ++k)
                    group[findGroup(list[k])] = findGroup(list[0]);
                uint32 const at = uint32(key >> 16);
                uint32 const toCluster = uint32(key & 0xFFFF);
                if (polys[at].second->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    continue;  // an off-mesh link is its own entrance
                forLinks(at,
                         [&](dtLink const& link)
                         {
                             int64 const j = local(link.ref);
                             if (j < 0)
                                 return;
                             auto const other = exitsAt.find((uint64(j) << 16) | toCluster);
                             if (other != exitsAt.end() &&
                                 polys[size_t(j)].second->getType() != DT_POLYTYPE_OFFMESH_CONNECTION)
                                 group[findGroup(other->second[0])] = findGroup(list[0]);
                         });
            }
            std::unordered_map<uint32, std::vector<uint32>> groups;
            for (uint32 e = 0; e < exits.size(); ++e)
                groups[findGroup(e)].push_back(e);

            std::unordered_map<uint32, std::vector<uint32>> nodesAt;  // local poly -> nodes of this cluster there
            std::vector<uint32> clusterNodes;
            std::unordered_map<uint32, uint32> nodePoly;  // node -> its local poly
            auto const addNode = [&](uint32 poly, G3D::Vector3 const& pos)
            {
                Node node;
                node.pos = pos;
                node.cluster = index;
                node.region = region[poly];
                uint32 const id = uint32(_graph->nodes.size());
                _graph->nodes.push_back(node);
                nodesAt[poly].push_back(id);
                nodePoly[id] = poly;
                clusterNodes.push_back(id);
                _graph->regionNodes[RegionKey(index, node.region)].push_back(id);
                _graph->clusterNodes[index].push_back(id);
                return id;
            };
            for (auto const& [root, members] : groups)
            {
                G3D::Vector3 centre(0.0f, 0.0f, 0.0f);
                for (uint32 e : members)
                    centre += exits[e].pos;
                centre /= float(members.size());
                uint32 rep = members[0];
                for (uint32 e : members)
                    if ((exits[e].pos - centre).squaredLength() < (exits[rep].pos - centre).squaredLength())
                        rep = e;
                uint32 const id = addNode(exits[rep].poly, exits[rep].pos);
                for (uint32 e : members)
                {
                    auto const [tile, poly] = polys[exits[e].poly];
                    _exitNode[ExitKey(PolyKey(tile->header, uint32(poly - tile->polys)), exits[e].toCluster)] = id;
                    _crossings.push_back({id, 0, index});
                    dtMeshTile const* nextTile = nullptr;
                    dtPoly const* next = nullptr;
                    _mesh->getTileAndPolyByRefUnsafe(exits[e].to, &nextTile, &next);
                    _crossings.back().toPoly = PolyKey(nextTile->header, uint32(next - nextTile->polys));
                }
            }
            // the landings of the neighbours' off-mesh links in this cluster (a one-way link has no exit back)
            for (uint32 other = 0; other < _clusters.size(); ++other)
            {
                if (other == index || !_clusters[other].loaded)
                    continue;
                for (dtTileRef ref : _clusters[other].tiles)
                {
                    dtMeshTile const* tile = _mesh->getTileByRef(ref);
                    if (!tile || !tile->header || !tile->header->offMeshConCount)
                        continue;
                    for (int i = tile->header->offMeshBase; i < tile->header->polyCount; ++i)
                    {
                        dtPoly const* poly = &tile->polys[i];
                        if (poly->getType() != DT_POLYTYPE_OFFMESH_CONNECTION || !Walkable(poly))
                            continue;
                        for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                        {
                            dtLink const& link = tile->links[l];
                            int64 const j = local(link.ref);
                            if (j < 0 || !walkable[size_t(j)])
                                continue;
                            auto const [landTile, landPoly] = polys[size_t(j)];
                            uint64 const key =
                                ExitKey(PolyKey(landTile->header, uint32(landPoly - landTile->polys)), other);
                            if (_exitNode.count(key))
                                continue;
                            _exitNode[key] = addNode(uint32(j), PortalPos(_mesh, tile, poly, link));
                        }
                    }
                }
            }

            // the walks between the cluster's nodes: a Dijkstra over its polygons from each node
            if (clusterNodes.size() >= 2)
            {
                std::vector<float> cost(total);
                std::vector<G3D::Vector3> at(total);
                std::vector<uint32> stamp(total, 0);
                std::vector<uint8> done(total, 0);
                uint32 run = 0;
                using Open = std::pair<float, uint32>;
                std::vector<Open> open;
                for (uint32 source : clusterNodes)
                {
                    Node const& from = _graph->nodes[source];
                    uint32 wanted = 0;
                    for (uint32 n : _graph->regionNodes[RegionKey(index, from.region)])
                        if (n != source)
                            ++wanted;
                    if (!wanted)
                        continue;
                    uint32 const startPoly = nodePoly[source];
                    ++run;
                    open.clear();
                    stamp[startPoly] = run;
                    cost[startPoly] = 0.0f;
                    at[startPoly] = from.pos;
                    done[startPoly] = 0;
                    open.emplace_back(0.0f, startPoly);
                    while (!open.empty() && wanted)
                    {
                        std::pop_heap(open.begin(), open.end(), std::greater<Open>());
                        auto const [g, u] = open.back();
                        open.pop_back();
                        if (done[u] || g > cost[u])
                            continue;
                        done[u] = 1;
                        ++_expanded;
                        if (auto const here = nodesAt.find(u); here != nodesAt.end())
                            for (uint32 n : here->second)
                                if (n != source)
                                {
                                    _walks.emplace_back(source, n, g + (_graph->nodes[n].pos - at[u]).length());
                                    --wanted;
                                }
                        auto const [tile, poly] = polys[u];
                        for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                        {
                            dtLink const& link = tile->links[l];
                            int64 const j = local(link.ref);
                            if (j < 0 || !walkable[size_t(j)])
                                continue;
                            uint32 const v = uint32(j);
                            G3D::Vector3 const portal = PortalPos(_mesh, tile, poly, link);
                            float const next = g + (portal - at[u]).length();
                            if (stamp[v] == run && (done[v] || next >= cost[v]))
                                continue;
                            if (stamp[v] != run)
                                done[v] = 0;
                            stamp[v] = run;
                            cost[v] = next;
                            at[v] = portal;
                            open.emplace_back(next, v);
                            std::push_heap(open.begin(), open.end(), std::greater<Open>());
                        }
                    }
                }
            }
            for (dtTileRef ref : cluster.tiles)
                _tileBase[_mesh->decodePolyIdTile(ref)] = -1;
        }

        std::unique_ptr<MapGraph> Builder::Build()
        {
            std::string const meshFile = Acore::StringFormat("{}mmaps/{:03}.mmap", _dataPath, _mapId);
            FILE* file = fopen(meshFile.c_str(), "rb");
            if (!file)
                return nullptr;
            dtNavMeshParams params;
            bool const read = fread(&params, sizeof(dtNavMeshParams), 1, file) == 1;
            fclose(file);
            if (!read)
                return nullptr;
            _mesh = dtAllocNavMesh();
            if (!_mesh || dtStatusFailed(_mesh->init(&params)))
                return nullptr;
            _tileOwner.assign(size_t(_mesh->getMaxTiles()), -1);
            _tileBase.assign(size_t(_mesh->getMaxTiles()), -1);

            std::string const prefix = Acore::StringFormat("{:03}", _mapId);
            std::error_code error;
            for (auto const& entry : std::filesystem::directory_iterator(_dataPath + "mmaps", error))
            {
                std::string const name = entry.path().filename().string();
                if (name.size() != 14 || name.compare(0, 3, prefix) != 0 || name.compare(7, 7, ".mmtile") != 0)
                    continue;
                Cluster cluster;
                cluster.a = std::atoi(name.substr(3, 2).c_str());
                cluster.b = std::atoi(name.substr(5, 2).c_str());
                _clusters.push_back(cluster);
            }
            if (_clusters.empty())
                return nullptr;
            _graph = std::make_unique<MapGraph>();
            _graph->mapId = _mapId;
            // three rows of grids loaded at a time (a grid's links reach only its neighbours)
            std::map<int, std::vector<uint32>> rows;
            for (uint32 i = 0; i < _clusters.size(); ++i)
                rows[_clusters[i].b].push_back(i);
            for (auto const& [row, members] : rows)
            {
                if (stopBuild.load())
                    return nullptr;
                for (uint32 i = 0; i < _clusters.size(); ++i)
                {
                    int const b = _clusters[i].b;
                    if (b >= row - 1 && b <= row + 1)
                        Load(i);
                    else if (_clusters[i].loaded)
                        Unload(i);
                }
                for (uint32 i : members)
                    Process(i);
            }
            // the crossings into the node of the polygon entered (its own exit back, or its off-mesh landing)
            uint32 crossings = 0;
            for (Crossing const& crossing : _crossings)
            {
                auto const to = _exitNode.find(ExitKey(crossing.toPoly, crossing.fromCluster));
                if (to == _exitNode.end() || to->second == crossing.from)
                    continue;
                _walks.emplace_back(crossing.from, to->second,
                                    (_graph->nodes[to->second].pos - _graph->nodes[crossing.from].pos).length());
                ++crossings;
            }
            std::sort(_walks.begin(), _walks.end());
            _walks.erase(std::unique(_walks.begin(), _walks.end(),
                                     [](auto const& x, auto const& y)
                                     { return std::get<0>(x) == std::get<0>(y) && std::get<1>(x) == std::get<1>(y); }),
                         _walks.end());
            _graph->edges.reserve(_walks.size());
            for (auto const& [from, to, yd] : _walks)
            {
                Node& node = _graph->nodes[from];
                if (!node.edgeCount)
                    node.firstEdge = uint32(_graph->edges.size());
                ++node.edgeCount;
                _graph->edges.push_back({to, yd});
            }
            LOG_INFO("playerbots", "[nav180] walk graph map {}: {} grids, {} polygons in {} regions, {} nodes, {} edges"
                     " ({} crossings), {} polygons expanded",
                     _mapId, _clusters.size(), _polys, _regions, _graph->nodes.size(), _graph->edges.size(), crossings,
                     _expanded);
            return std::move(_graph);
        }

        void BuildAll(std::string const dataPath)
        {
            for (size_t slot = 0; slot < GRAPH_MAPS.size(); ++slot)
            {
                if (stopBuild.load())
                    return;
                auto const started = std::chrono::steady_clock::now();
                std::unique_ptr<MapGraph> graph;
                // [nav184] an exception out of a std::thread's function terminates the server: a failed build is
                // logged and the map walks without its graph
                try
                {
                    graph = Builder(GRAPH_MAPS[slot], dataPath).Build();
                }
                catch (std::exception const& error)
                {
                    LOG_ERROR("playerbots", "[nav184] walk graph map {}: build failed: {}", GRAPH_MAPS[slot],
                              error.what());
                    graph.reset();
                }
                uint64 const ms = uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count());
                if (!graph)
                {
                    LOG_INFO("playerbots", "[nav180] walk graph map {}: not built ({} ms)", GRAPH_MAPS[slot], ms);
                    continue;
                }
                LOG_INFO("playerbots", "[nav180] walk graph map {} ready in {} ms", GRAPH_MAPS[slot], ms);
                owned[slot] = std::move(graph);
                published[slot].store(owned[slot].get());
            }
        }

        dtNavMeshQuery* LiveQuery(dtNavMesh const* mesh)
        {
            struct Free
            {
                void operator()(dtNavMeshQuery* query) const { dtFreeNavMeshQuery(query); }
            };
            thread_local std::unordered_map<dtNavMesh const*, std::unique_ptr<dtNavMeshQuery, Free>> queries;
            auto& query = queries[mesh];
            if (!query)
            {
                query.reset(dtAllocNavMeshQuery());
                if (!query || dtStatusFailed(query->init(mesh, 64)))  // nearest-polygon lookups only
                {
                    query.reset();
                    return nullptr;
                }
            }
            return query.get();
        }

        // the cluster and region of the polygon at `at` on the live navmesh (region NO_REGION: no polygon there, or
        // its tile not loaded: the cluster from the tile's place alone)
        bool Locate(MapGraph const& graph, dtNavMesh const* mesh, G3D::Vector3 const& at, uint32& cluster,
                    uint16& region)
        {
            region = NO_REGION;
            float const point[3] = {at.y, at.z, at.x};
            if (dtNavMeshQuery* query = LiveQuery(mesh))
            {
                dtQueryFilter filter;
                filter.setIncludeFlags(WALK_INCLUDE);
                filter.setExcludeFlags(WALK_EXCLUDE);
                float nearest[3];
                dtPolyRef ref = 0;
                float extents[3] = {3.0f, 5.0f, 3.0f};  // the route search's own boxes (LongRoute)
                if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) || !ref)
                {
                    extents[1] = 50.0f;
                    ref = 0;
                    query->findNearestPoly(point, extents, &filter, &ref, nearest);
                }
                dtMeshTile const* tile = nullptr;
                dtPoly const* poly = nullptr;
                if (ref && dtStatusSucceed(mesh->getTileAndPolyByRef(ref, &tile, &poly)) && tile->header)
                {
                    auto const found =
                        graph.tiles.find(TileKey(tile->header->x, tile->header->y, tile->header->layer));
                    size_t const index = size_t(poly - tile->polys);
                    if (found != graph.tiles.end() && index < found->second.region.size())
                    {
                        cluster = found->second.cluster;
                        region = found->second.region[index];
                        return true;
                    }
                }
            }
            int tx = 0;
            int ty = 0;
            mesh->calcTileLoc(point, &tx, &ty);
            auto const found = graph.tileCluster.find(TileKey(tx, ty, 0));
            if (found == graph.tileCluster.end())
                return false;
            cluster = found->second;
            return true;
        }

        struct SearchStats
        {
            uint64 us = 0;
            uint64 count = 0;
            uint64 maxUs = 0;
        };
    }

    void StartBuild()
    {
        if (builder.joinable())
            return;
        stopBuild.store(false);
        std::string const dataPath = sWorld->GetDataPath();
        builder = std::thread(BuildAll, dataPath);
    }

    void StopBuild()
    {
        stopBuild.store(true);
        if (builder.joinable())
            builder.join();
    }

    bool LiveTileLoaded(Map* map, G3D::Vector3 const& at)
    {
        dtNavMesh const* mesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!mesh)
            return false;
        float const point[3] = {at.y, at.z, at.x};
        int tx = 0;
        int ty = 0;
        mesh->calcTileLoc(point, &tx, &ty);
        return tx >= 0 && ty >= 0 && mesh->getTileAt(tx, ty, 0) != nullptr;
    }

    Plan PlanWalk(Map* map, G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<uint32> const& banned)
    {
        Plan plan;
        auto const started = std::chrono::steady_clock::now();
        int const slot = map && !map->Instanceable() ? SlotOf(map->GetId()) : -1;
        MapGraph const* graph = slot >= 0 ? published[size_t(slot)].load() : nullptr;
        dtNavMesh const* mesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!graph || !mesh)
            return plan;
        plan.ready = true;
        uint32 fromCluster = 0;
        uint32 toCluster = 0;
        uint16 fromRegion = NO_REGION;
        uint16 toRegion = NO_REGION;
        if (!Locate(*graph, mesh, from, fromCluster, fromRegion) || fromRegion == NO_REGION ||
            !Locate(*graph, mesh, to, toCluster, toRegion))
            return plan;
        if (fromCluster == toCluster && (fromRegion == toRegion || toRegion == NO_REGION))
        {
            plan.sameRegion = true;
            return plan;
        }
        auto const starts = graph->regionNodes.find(RegionKey(fromCluster, fromRegion));
        std::vector<uint32> const* goals = nullptr;
        if (toRegion != NO_REGION)
        {
            auto const found = graph->regionNodes.find(RegionKey(toCluster, toRegion));
            goals = found != graph->regionNodes.end() ? &found->second : nullptr;
        }
        else
        {
            auto const found = graph->clusterNodes.find(toCluster);
            goals = found != graph->clusterNodes.end() ? &found->second : nullptr;
        }
        if (starts == graph->regionNodes.end() || !goals)
            return plan;

        // A* over the nodes (straight lines: every edge is at least its straight line)
        size_t const count = graph->nodes.size();
        // per thread and map: the arrays sized to the graph, a run stamp instead of clearing them
        struct Scratch
        {
            std::vector<float> g;
            std::vector<uint32> parent;
            std::vector<uint32> seen;
            std::vector<uint32> mark;
            std::vector<uint8> flags;
            uint32 run = 0;
        };
        thread_local std::array<Scratch, GRAPH_MAPS.size()> scratches;
        Scratch& scratch = scratches[size_t(slot)];
        if (scratch.g.size() != count)
        {
            scratch.g.assign(count, 0.0f);
            scratch.parent.assign(count, NO_NODE);
            scratch.seen.assign(count, 0);
            scratch.mark.assign(count, 0);
            scratch.flags.assign(count, 0);
            scratch.run = 0;
        }
        uint32 const run = ++scratch.run;
        std::vector<float>& g = scratch.g;
        std::vector<uint32>& parent = scratch.parent;
        std::vector<uint32>& seen = scratch.seen;
        std::vector<uint32>& mark = scratch.mark;
        std::vector<uint8>& flags = scratch.flags;
        constexpr uint8 CLOSED = 1;
        constexpr uint8 GOAL = 2;
        constexpr uint8 BANNED = 4;
        auto const flagsOf = [&](uint32 n) -> uint8& {
            if (mark[n] != run)
            {
                mark[n] = run;
                flags[n] = 0;
            }
            return flags[n];
        };
        for (uint32 n : banned)
            if (n < count)
                flagsOf(n) |= BANNED;
        for (uint32 n : *goals)
            flagsOf(n) |= GOAL;
        using Open = std::pair<float, uint32>;
        thread_local std::vector<Open> open;
        open.clear();
        auto const push = [&](uint32 n, float cost, uint32 via)
        {
            if (seen[n] == run && cost >= g[n])
                return;
            seen[n] = run;
            g[n] = cost;
            parent[n] = via;
            open.emplace_back(cost + (graph->nodes[n].pos - to).length(), n);
            std::push_heap(open.begin(), open.end(), std::greater<Open>());
        };
        for (uint32 n : starts->second)
            if (!(flagsOf(n) & BANNED))
                push(n, (graph->nodes[n].pos - from).length(), NO_NODE);
        float best = std::numeric_limits<float>::max();
        uint32 bestNode = NO_NODE;
        while (!open.empty())
        {
            std::pop_heap(open.begin(), open.end(), std::greater<Open>());
            auto const [f, n] = open.back();
            open.pop_back();
            if (f >= best)
                break;
            uint8& state = flagsOf(n);
            if (state & CLOSED)
                continue;
            state |= CLOSED;
            if (state & GOAL)
                if (float const total = g[n] + (graph->nodes[n].pos - to).length(); total < best)
                {
                    best = total;
                    bestNode = n;
                }
            Node const& node = graph->nodes[n];
            for (uint32 e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e)
            {
                Edge const& edge = graph->edges[e];
                if (flagsOf(edge.to) & (BANNED | CLOSED))
                    continue;
                push(edge.to, g[n] + edge.yd, n);
            }
        }
        if (bestNode != NO_NODE)
        {
            for (uint32 n = bestNode; n != NO_NODE; n = parent[n])
                plan.nodes.push_back(n);
            std::reverse(plan.nodes.begin(), plan.nodes.end());
            for (uint32 n : plan.nodes)
            {
                plan.points.push_back(graph->nodes[n].pos);
                plan.walked.push_back(g[n]);
            }
            plan.yd = best;
            plan.found = true;
        }
        plan.us = uint64(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        return plan;
    }

    void NoteSearch(SearchKind kind, uint64 us, ObjectGuid walker)
    {
        constexpr uint32 WINDOW_MS = 10 * MINUTE * IN_MILLISECONDS;  // a log cadence
        static std::mutex lock;
        static std::array<SearchStats, size_t(SearchKind::Count)> stats;
        static std::unordered_set<uint64> walkers;
        static uint32 windowStart = 0;
        std::lock_guard<std::mutex> guard(lock);
        uint32 const now = getMSTime();
        if (!windowStart)
            windowStart = now;
        SearchStats& one = stats[size_t(kind)];
        one.us += us;
        ++one.count;
        one.maxUs = std::max(one.maxUs, us);
        walkers.insert(walker.GetRawValue());
        uint32 const elapsed = getMSTimeDiff(windowStart, now);
        if (elapsed < WINDOW_MS)
            return;
        double const botHours = std::max(1.0, double(walkers.size())) * double(elapsed) / 3600000.0;
        auto const perBotHour = [&](SearchKind k) { return double(stats[size_t(k)].us) / 1000.0 / botHours; };
        auto const avg = [&](SearchKind k)
        {
            SearchStats const& s = stats[size_t(k)];
            return s.count ? double(s.us) / double(s.count) / 1000.0 : 0.0;
        };
        LOG_INFO("playerbots",
                 "[nav183][perf] far walk searches in {} s, {} walkers: per bot and hour full {:.0f} ms ({} searches, "
                 "avg {:.1f} ms, max {:.1f}), legs {:.0f} ms ({}, avg {:.1f}), graph stages {:.0f} ms ({}, avg {:.1f}, "
                 "max {:.1f}), graph plans {:.2f} ms ({}, avg {:.0f} us, max {} us); fine {:.0f} ms vs graph {:.0f} ms",
                 elapsed / IN_MILLISECONDS, walkers.size(), perBotHour(SearchKind::Full),
                 stats[size_t(SearchKind::Full)].count, avg(SearchKind::Full),
                 double(stats[size_t(SearchKind::Full)].maxUs) / 1000.0, perBotHour(SearchKind::Leg),
                 stats[size_t(SearchKind::Leg)].count, avg(SearchKind::Leg), perBotHour(SearchKind::Stage),
                 stats[size_t(SearchKind::Stage)].count, avg(SearchKind::Stage),
                 double(stats[size_t(SearchKind::Stage)].maxUs) / 1000.0, perBotHour(SearchKind::Graph),
                 stats[size_t(SearchKind::Graph)].count, avg(SearchKind::Graph) * 1000.0,
                 stats[size_t(SearchKind::Graph)].maxUs, perBotHour(SearchKind::Full),
                 perBotHour(SearchKind::Stage) + perBotHour(SearchKind::Graph));
        stats = {};
        walkers.clear();
        windowStart = now;
    }
}
