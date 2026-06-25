#pragma once

#include "SDL3/SDL_pixels.h"
#include "types/sdl_handle_defs.h"
#include "types/bounding_box.h"
#include <mutex>
#include <filesystem>
#include <string>

namespace spintool::rom
{
	class SpinballROM;

	struct Sprite;
	struct Palette;
	struct PaletteSet;
	struct SpriteTile;
}

namespace spintool
{
	class Renderer
	{
	public:
		static bool Initialise();
		static void Shutdown();
		static void NewFrame();
		static void Render();

		// Apply SpinTool's modern violet-to-blue interface styling.
		static void ApplyModernTheme();

		// Font changes are queued and applied safely before the next ImGui frame.
		// Passing an empty path restores Dear ImGui's built-in default font.
		static void RequestFont(const std::filesystem::path& font_path);
		[[nodiscard]] static std::filesystem::path GetActiveFontPath();
		[[nodiscard]] static std::string GetFontError();

		static SDLPaletteHandle CreateSDLPalette(const rom::Palette& palette);
		static SDLPaletteHandle CreateSDLPaletteForSet(const rom::PaletteSet& palette_set);
		static void SetPalette(const SDLPaletteHandle& palette);

		static SDLTextureHandle RenderToTexture(const rom::Sprite& sprite, bool flip_x = false, bool flip_y = false);
		static SDLTextureHandle RenderToTexture(const rom::SpriteTile& sprite_tile);
		static SDLTextureHandle RenderToTexture(SDL_Surface* surface);
		static SDLTextureHandle RenderArbitaryOffsetToTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions);
		static SDLTextureHandle RenderArbitaryOffsetToTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions, const rom::Palette& palette);
		static SDLTextureHandle RenderArbitaryOffsetToTilesetTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions_in_tiles);

		static SDL_Renderer* s_renderer;
		static SDL_Window* s_window;
		static SDLPaletteHandle s_current_palette;

		static std::recursive_mutex s_sdl_update_mutex;

		constexpr static const int s_window_width = 1600;
		constexpr static const int s_window_height = 900;
	};
}
