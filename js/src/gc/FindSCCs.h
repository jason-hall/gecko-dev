/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_FindSCCs_h
#define gc_FindSCCs_h

#include "mozilla/Move.h"

#include "jsfriendapi.h"
#include "jsutil.h"

namespace js {
namespace gc {

template<class Node>
struct GraphNodeBase
{
    Node*          gcNextGraphNode;
    Node*          gcNextGraphComponent;
    unsigned       gcDiscoveryTime;
    unsigned       gcLowLink;

    GraphNodeBase()
      : gcNextGraphNode(nullptr),
        gcNextGraphComponent(nullptr),
        gcDiscoveryTime(0),
        gcLowLink(0) {}

    ~GraphNodeBase() {}

    Node* nextNodeInGroup() const {
        if (gcNextGraphNode && gcNextGraphNode->gcNextGraphComponent == gcNextGraphComponent)
            return gcNextGraphNode;
        return nullptr;
    }

    Node* nextGroup() const {
        return gcNextGraphComponent;
    }
};

/*
 * Find the strongly connected components of a graph using Tarjan's algorithm,
 * and return them in topological order.
 *
 * Nodes derive from GraphNodeBase and implement findGraphEdges, which calls
 * finder.addEdgeTo to describe the outgoing edges from that node:
 *
 * struct MyComponentFinder;
 *
 * struct MyGraphNode : public GraphNodeBase
 * {
 *     void findOutgoingEdges(MyComponentFinder& finder)
 *     {
 *         for edge in my_outgoing_edges:
 *             if is_relevant(edge):
 *                 finder.addEdgeTo(edge.destination)
 *     }
 * }
 *
 * struct MyComponentFinder : public ComponentFinder<MyGraphNode, MyComponentFinder>
 * {
 *     ...
 * };
 *
 * MyComponentFinder finder;
 * finder.addNode(v);
 */

template <typename Node, typename Derived>
class ComponentFinder
{
  public:
    explicit ComponentFinder(uintptr_t sl) {}

    /* Forces all nodes to be added to a single component. */
    void useOneComponent() { }

    void addNode(Node* v) {
    }

    Node* getResultsList() {
		return nullptr;
    }

  public:
    /* Call from implementation of GraphNodeBase::findOutgoingEdges(). */
    void addEdgeTo(Node* w) {
    }
};

} /* namespace gc */
} /* namespace js */

#endif /* gc_FindSCCs_h */
