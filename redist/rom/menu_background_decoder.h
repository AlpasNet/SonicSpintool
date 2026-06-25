#pragma once

#include "SDL3/SDL_stdinc.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace spintool::rom
{
	class SpinballROM;
	struct Sprite;
	struct PaletteSet;

	enum class MenuBackgroundCategory : Uint8
	{
		OPTIONS = 0,
		HI_SCORE = 1,
		CREDITS = 2,
	};

	struct MenuBackgroundFrame
	{
		MenuBackgroundCategory category = MenuBackgroundCategory::OPTIONS;
		std::string name;
		std::string usage;
		std::size_t frame_id = 0;
		std::shared_ptr<const Sprite> sprite;
		std::vector<Uint8> palette_line_map;
	};

	struct MenuBackgroundDecodeResult
	{
		std::vector<MenuBackgroundFrame> frames;
		std::shared_ptr<const PaletteSet> palette_set;
		std::vector<std::string> warnings;
		std::string error;

		[[nodiscard]] bool Succeeded() const
		{
			return error.empty() && !frames.empty();
		}
	};

	struct MenuBackgroundImportResult
	{
		bool success = false;
		bool changed = false;
		std::size_t remaining_bytes = 0U;
		std::size_t compression_saved_bytes = 0U;
		std::string message;
	};

	class MenuBackgroundDecoder
	{
	public:
		// The Options, Hi-Score and Credits screens all reference this same
		// 320x224 background. A single decoded frame is returned for the common
		// ROM resource.
		static MenuBackgroundDecodeResult Decode(const SpinballROM& rom);

		// Rebuilds the shared background as editable 8x8 cells, deduplicates only
		// identical patterns for VRAM safety, stores it in fixed reserved ROM
		// areas, and redirects all three screens to it.
		static MenuBackgroundImportResult ImportIndexedImage(
			SpinballROM& rom,
			const SpinballROM& reference_rom,
			const std::vector<Uint8>& indexed_pixels,
			int image_width,
			int image_height
		);

		// Current addresses used by the Tile Layout Viewer. They resolve either
		// the original resource or SpinTool's fixed relocated resource.
		static Uint32 ResolveArtHeaderOffset(const SpinballROM& rom);
		static Uint32 ResolveBrushesOffset(const SpinballROM& rom);
		static Uint32 ResolveLayoutOffset(const SpinballROM& rom);
		static Uint32 ResolveBrushesEndOffset(const SpinballROM& rom);
		static Uint32 ResolveLayoutEndOffset(const SpinballROM& rom);
	};
}
