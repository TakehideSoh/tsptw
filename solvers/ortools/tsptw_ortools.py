#!/usr/bin/env python3

import argparse
import time

from ortools.sat.python import cp_model

import read_tsptw


start = time.perf_counter()


class SolutionCallback(cp_model.CpSolverSolutionCallback):
    def __init__(self, history_file, start_time):
        cp_model.CpSolverSolutionCallback.__init__(self)
        self._history_file = history_file
        self._start_time = start_time

    def on_solution_callback(self):
        if self._history_file:
            with open(self._history_file, "a") as f:
                f.write(
                    "{}, {}\n".format(
                        time.perf_counter() - self._start_time,
                        self.ObjectiveValue(),
                    )
                )


def solve(nodes, edges, a, b, time_limit=None, threads=1, history=None):
    n = len(nodes)
    max_time = max(b.values()) + max(edges.get((i, j), 0) for i in nodes for j in nodes)

    model = cp_model.CpModel()

    # Position of each node in the tour (0 is always first)
    pos = [model.NewIntVar(0, n - 1, f"pos_{i}") for i in nodes]
    model.Add(pos[0] == 0)  # Depot is first

    # All positions are different
    model.AddAllDifferent(pos)

    # Arrival time at each node
    arrival = [model.NewIntVar(0, max_time, f"arrival_{i}") for i in nodes]
    model.Add(arrival[0] == 0)  # Start at time 0

    # Time windows
    for i in nodes:
        model.Add(arrival[i] >= a[i])
        model.Add(arrival[i] <= b[i])

    # Transition constraints: if node j comes right after node i, then arrival[j] >= arrival[i] + edges[i,j]
    for i in nodes:
        for j in nodes:
            if i != j and (i, j) in edges:
                # If pos[j] == pos[i] + 1
                i_before_j = model.NewBoolVar(f"i_{i}_before_j_{j}")
                model.Add(pos[j] == pos[i] + 1).OnlyEnforceIf(i_before_j)
                model.Add(pos[j] != pos[i] + 1).OnlyEnforceIf(i_before_j.Not())
                model.Add(arrival[j] >= arrival[i] + edges[i, j]).OnlyEnforceIf(i_before_j)

    # Compute total distance
    # For each position p (except last), find the edge cost from node at p to node at p+1
    total_cost = model.NewIntVar(0, sum(edges.values()), "total_cost")

    edge_costs = []
    for p in range(n - 1):
        for i in nodes:
            for j in nodes:
                if i != j and (i, j) in edges:
                    # Cost if node i is at position p and node j is at position p+1
                    i_at_p = model.NewBoolVar(f"node_{i}_at_pos_{p}")
                    j_at_p1 = model.NewBoolVar(f"node_{j}_at_pos_{p+1}")
                    both = model.NewBoolVar(f"edge_{i}_{j}_at_{p}")

                    model.Add(pos[i] == p).OnlyEnforceIf(i_at_p)
                    model.Add(pos[i] != p).OnlyEnforceIf(i_at_p.Not())
                    model.Add(pos[j] == p + 1).OnlyEnforceIf(j_at_p1)
                    model.Add(pos[j] != p + 1).OnlyEnforceIf(j_at_p1.Not())
                    model.AddBoolAnd([i_at_p, j_at_p1]).OnlyEnforceIf(both)
                    model.AddBoolOr([i_at_p.Not(), j_at_p1.Not()]).OnlyEnforceIf(both.Not())

                    edge_cost = model.NewIntVar(0, edges[i, j], f"cost_{i}_{j}_{p}")
                    model.Add(edge_cost == edges[i, j]).OnlyEnforceIf(both)
                    model.Add(edge_cost == 0).OnlyEnforceIf(both.Not())
                    edge_costs.append(edge_cost)

    # Add return to depot (from last node to node 0)
    for i in nodes:
        if i != 0 and (i, 0) in edges:
            i_at_last = model.NewBoolVar(f"node_{i}_at_last")
            model.Add(pos[i] == n - 1).OnlyEnforceIf(i_at_last)
            model.Add(pos[i] != n - 1).OnlyEnforceIf(i_at_last.Not())

            return_cost = model.NewIntVar(0, edges[i, 0], f"return_cost_{i}")
            model.Add(return_cost == edges[i, 0]).OnlyEnforceIf(i_at_last)
            model.Add(return_cost == 0).OnlyEnforceIf(i_at_last.Not())
            edge_costs.append(return_cost)

    model.Add(total_cost == sum(edge_costs))
    model.Minimize(total_cost)

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit if time_limit else 1800
    solver.parameters.num_search_workers = threads

    if history:
        with open(history, "w") as f:
            pass

    callback = SolutionCallback(history, start) if history else None
    status = solver.Solve(model, callback)

    print("Search time: {}s".format(solver.WallTime()))

    if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        pos_to_node = {solver.Value(pos[i]): i for i in nodes}
        solution = [pos_to_node[p] for p in range(n)]
        solution.append(0)  # Return to depot

        cost = solver.Value(total_cost)
        print(solution)
        print("cost: {}".format(cost))

        validation_result = read_tsptw.validate(n, edges, a, b, solution, cost)

        if validation_result:
            print("The solution is valid.")
            if status == cp_model.OPTIMAL:
                print("optimal cost: {}".format(cost))
            else:
                print("best bound: {}".format(int(solver.BestObjectiveBound())))
        else:
            print("The solution is invalid")
    elif status == cp_model.INFEASIBLE:
        print("The problem is infeasible.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=str)
    parser.add_argument("--threads", default=1, type=int)
    parser.add_argument("--time-out", default=1800, type=float)
    parser.add_argument("--history", type=str)
    args = parser.parse_args()

    _, nodes, edges, a, b = read_tsptw.read(args.input)
    solve(
        nodes,
        edges,
        a,
        b,
        time_limit=args.time_out,
        threads=args.threads,
        history=args.history,
    )
