// Copyright (c) 2026 DavDood. GPL-3.0-only; see ../LICENSE.
// Offline model checks, not an Explorer integration test.
#define SECONDARY_TASKBAR_ORDER_PLANNER_ONLY
#include "../src/secondary-taskbar-pinned-order.wh.cpp"

#include <cstdio>
#include <cstdlib>

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void Check(const std::vector<int>& initial, const std::vector<int>& pins) {
    // Independent oracle: project the pin list onto membership, then append
    // the original unpinned subsequence. No sorting algorithm in the oracle.
    std::vector<int> expected;
    for (int pin : pins) {
        for (int app : initial) {
            if (app == pin) {
                expected.push_back(app);
            }
        }
    }
    std::vector<int> unpinned;
    for (int app : initial) {
        if (std::find(pins.begin(), pins.end(), app) == pins.end()) {
            unpinned.push_back(app);
        }
    }
    expected.insert(expected.end(), unpinned.begin(), unpinned.end());

    auto current = initial;
    for (size_t moves = 0;; ++moves) {
        Require(moves <= current.size(), "planner did not converge within group count");
        std::vector<size_t> ranks;
        for (int app : current) {
            ranks.push_back(std::find(pins.begin(), pins.end(), app) - pins.begin());
        }
        const auto move = SecondaryTaskbarOrder::NextMove(ranks);
        if (move.to == current.size()) {
            break;
        }
        Require(move.to < move.from && move.from < current.size(), "move must be leftward");
        Require(ranks[move.from] < pins.size(), "only pinned groups may be moved");
        std::rotate(current.begin() + move.to, current.begin() + move.from,
                    current.begin() + move.from + 1);
        std::vector<int> remaining;
        for (int app : current) {
            if (std::find(pins.begin(), pins.end(), app) == pins.end()) {
                remaining.push_back(app);
            }
        }
        Require(remaining == unpinned, "unpinned relative order changed during a move");
    }
    Require(current == expected, "result differs from projected pins + stable unpinned oracle");
    Require(current.size() == initial.size(), "group membership changed");
}

int main() {
    size_t cases = 0;
    // Every opening/membership permutation of six distinct groups, with all
    // ordered pin subsets. Covers empty/no-pin/all-pin/missing-pin cases and
    // applying a changed pin list to each possible current taskbar order.
    for (unsigned pinMask = 0; pinMask < 64; ++pinMask) {
        std::vector<int> pins;
        for (int app = 0; app < 6; ++app) {
            if (pinMask & (1u << app)) pins.push_back(app);
        }
        do {
            for (unsigned shownMask = 0; shownMask < 64; ++shownMask) {
                std::vector<int> shown;
                for (int app = 0; app < 6; ++app) {
                    if (shownMask & (1u << app)) shown.push_back(app);
                }
                do {
                    Check(shown, pins);
                    ++cases;
                } while (std::next_permutation(shown.begin(), shown.end()));
            }
        } while (std::next_permutation(pins.begin(), pins.end()));
    }
    // Equal ranks remain stable, including an identity represented twice.
    Require(SecondaryTaskbarOrder::NextMove({}).to == 0, "empty planner result");
    Check({5, 1, 1, 0, 4}, {0, 1});
    Check({0, 1, 4, 5}, {});
    std::printf("PASS: %zu exhaustive cases plus equal-rank/no-pin checks\n", cases);
}
