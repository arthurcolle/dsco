#!/usr/bin/env python3
"""Dijkstra and Bellman-Ford shortest path algorithms with unit tests."""

import heapq
from typing import Dict, List, Tuple, Optional


def dijkstra(graph, start):
    """
    Dijkstra's algorithm for shortest paths.
    Returns dict of {node: shortest_distance} from start.
    Does not handle negative edge weights.
    """
    distances = {node: float('inf') for node in graph}
    distances[start] = 0
    
    pq = [(0, start)]
    visited = set()
    
    while pq:
        dist, node = heapq.heappop(pq)
        
        if node in visited:
            continue
        visited.add(node)
        
        for neighbor, weight in graph.get(node, []):
            if neighbor not in distances:
                distances[neighbor] = float('inf')
            
            new_dist = dist + weight
            if new_dist < distances[neighbor]:
                distances[neighbor] = new_dist
                heapq.heappush(pq, (new_dist, neighbor))
    
    return distances


def bellman_ford(graph, start):
    """
    Bellman-Ford algorithm for shortest paths.
    Handles negative edge weights and detects negative cycles.
    
    Returns:
        (distances, has_negative_cycle)
        - distances: dict of {node: shortest_distance} or None if negative cycle
        - has_negative_cycle: True if negative cycle detected
    """
    nodes = set(graph.keys())
    for node in graph:
        for neighbor, _ in graph[node]:
            nodes.add(neighbor)
    
    distances = {node: float('inf') for node in nodes}
    distances[start] = 0
    
    for _ in range(len(nodes) - 1):
        for node in graph:
            for neighbor, weight in graph[node]:
                if distances[node] + weight < distances[neighbor]:
                    distances[neighbor] = distances[node] + weight
    
    has_negative_cycle = False
    for node in graph:
        for neighbor, weight in graph[node]:
            if distances[node] + weight < distances[neighbor]:
                has_negative_cycle = True
                break
    
    if has_negative_cycle:
        return None, True
    
    return distances, False


def create_original_graph():
    """Original graph: {A: [(B,3),(C,7)], B: [(C,1),(D,5)], C: [(D,2)], D: [(A,4)]}"""
    return {
        'A': [('B', 3), ('C', 7)],
        'B': [('C', 1), ('D', 5)],
        'C': [('D', 2)],
        'D': [('A', 4)]
    }


def create_modified_graph():
    """Modified graph: original + edge D->B with weight -2"""
    graph = create_original_graph()
    graph['D'].append(('B', -2))
    return graph


def create_negative_cycle_graph():
    """Graph with a true negative cycle: A->B->C->A with total weight -1"""
    return {
        'A': [('B', 1)],
        'B': [('C', -2)],
        'C': [('A', 0)]
    }


def run_tests():
    """Run all unit tests."""
    print("=" * 60)
    print("GRAPH ALGORITHMS UNIT TESTS")
    print("=" * 60)
    
    print("\n[TEST 1] Dijkstra shortest paths from A on original graph")
    original = create_original_graph()
    result = dijkstra(original, 'A')
    expected = {'A': 0, 'B': 3, 'C': 4, 'D': 6}
    print(f"  Result: {result}")
    print(f"  Expected: {expected}")
    assert result['A'] == 0
    assert result['B'] == 3
    assert result['C'] == 4
    assert result['D'] == 6
    print("  PASSED")
    
    print("\n[TEST 2] Bellman-Ford shortest paths from A on original graph")
    distances, has_cycle = bellman_ford(original, 'A')
    print(f"  Result: {distances}")
    print(f"  Negative cycle: {has_cycle}")
    assert has_cycle == False
    assert distances['A'] == 0
    assert distances['B'] == 3
    assert distances['C'] == 4
    assert distances['D'] == 6
    print("  PASSED")
    
    print("\n[TEST 3] Bellman-Ford on modified graph (D->B weight=-2)")
    modified = create_modified_graph()
    distances, has_cycle = bellman_ford(modified, 'A')
    print(f"  Graph: A->B(3), A->C(7), B->C(1), B->D(5), C->D(2), D->A(4), D->B(-2)")
    print(f"  Result: {distances}")
    print(f"  Negative cycle: {has_cycle}")
    assert has_cycle == False
    assert distances['A'] == 0
    assert distances['B'] == 3
    assert distances['C'] == 4
    assert distances['D'] == 6
    print("  PASSED")
    
    print("\n[TEST 4] Bellman-Ford detects true negative cycle")
    neg_cycle = create_negative_cycle_graph()
    distances, has_cycle = bellman_ford(neg_cycle, 'A')
    print(f"  Graph: A->B(1), B->C(-2), C->A(0)")
    print(f"  Cycle weight: 1 + (-2) + 0 = -1 (NEGATIVE)")
    print(f"  Result: {distances}")
    print(f"  Negative cycle detected: {has_cycle}")
    assert has_cycle == True
    assert distances is None
    print("  PASSED")
    
    print("\n" + "=" * 60)
    print("ALL TESTS PASSED")
    print("=" * 60)


if __name__ == '__main__':
    run_tests()
