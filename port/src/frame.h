// The frame's display list on the host: what Ninja and Kamui draw for a render, list by list. Ninja
// writes each 32-byte parameter into the store queues at its list's pointer and flushes it, as it
// did for the tile accelerator. Each list's pointer starts in its own range of store-queue
// addresses, so a flush in a list's range adds the parameter to that list and goes nowhere else.
// The parameters keep the tile accelerator's format, which the renderer decodes (render/ta.h).
#pragma once

#include <cstdint>
#include <vector>

#include "render/ta.h"

namespace frame {

// A render's lists: each list's parameters, in the order they came.
struct Lists {
    std::vector<uint32_t> list[render::kLists];
};

// Starts the lists of a new frame.
void begin();

// The store-queue address where list `n`'s parameters start.
uint32_t start(unsigned n);

// Whether `address` is in a list's range of store-queue addresses.
bool in_lists(uint32_t address);

// A flushed store queue at `address` in a list's range, 32 bytes at `words`, for the list.
void flush(uint32_t address, const uint8_t* words);

// The frame's lists, which the next frame starts anew.
Lists take();

// The lists as one stream for the renderer: each list in turn, closed by an end of list.
std::vector<uint32_t> stream(const Lists& lists);

// A hash of what a decoded display list draws: each list's strips with their state and vertices.
uint64_t hash(const render::DisplayList& list);

}  // namespace frame
