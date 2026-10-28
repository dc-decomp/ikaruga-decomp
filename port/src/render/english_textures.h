// ENGLISH TEXT for the textures that hold Japanese words as pictures: the choice of the game mode
// and of the difficulty, and the motto at the start of a game. The renderer knows each by the hash
// of its bytes in video memory (gpu_metal.mm), and English goes into the texels that the game draws
// each phrase from.
#pragma once

#include <cstdint>
#include <vector>

namespace render {

// Whether a texture with the hash `hash` has English.
bool has_english(uint64_t hash);

// Writes the English into the decoded texture, RGBA8 with red in the lowest byte, `width` texels a
// row.
void write_english(uint64_t hash, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba);

}  // namespace render
