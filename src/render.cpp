#include "render.h"

#include <iostream>
#include <fstream>
#include <optional>
#include <vector>
#include <limits>

#include "rom/spinball_rom.h"
#include "rom/sprite.h"
#include "rom/palette.h"

#define SDL_ENABLE_OLD_NAMES
#include "SDL3/SDL.h"
#include "SDL3/SDL_image.h"
#include "SDL3/SDL_oldnames.h"

#include "imgui.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "backends/imgui_impl_sdl3.h"
#include "ui/ui_palette.h"

namespace
{
	std::optional<std::filesystem::path> g_pending_font_path;
	std::filesystem::path g_active_font_path;
	std::string g_font_error;
	std::vector<unsigned char> g_font_data;

	constexpr float k_custom_font_size = 18.0f;

	bool LoadFontFile(const std::filesystem::path& font_path)
	{
		std::ifstream stream(font_path, std::ios::binary | std::ios::ate);
		if (!stream.is_open())
		{
			g_font_error = "Could not open font file: " + font_path.string();
			return false;
		}

		const std::streamoff file_size = stream.tellg();
		if (file_size <= 0 ||
			file_size > static_cast<std::streamoff>(std::numeric_limits<int>::max()))
		{
			g_font_error = "Invalid font file size: " + font_path.string();
			return false;
		}

		g_font_data.resize(static_cast<size_t>(file_size));
		stream.seekg(0, std::ios::beg);
		if (!stream.read(
			reinterpret_cast<char*>(g_font_data.data()),
			static_cast<std::streamsize>(g_font_data.size())
		))
		{
			g_font_data.clear();
			g_font_error = "Could not read font file: " + font_path.string();
			return false;
		}

		return true;
	}

	void ApplyPendingFontRequest()
	{
		if (!g_pending_font_path.has_value())
		{
			return;
		}

		const std::filesystem::path requested_path = *g_pending_font_path;
		g_pending_font_path.reset();

		ImGuiIO& io = ImGui::GetIO();
		ImGui_ImplSDLRenderer3_DestroyFontsTexture();
		io.Fonts->Clear();
		g_font_data.clear();
		g_font_error.clear();
		g_active_font_path.clear();

		ImFont* loaded_font = nullptr;
		if (!requested_path.empty() && LoadFontFile(requested_path))
		{
			ImFontConfig config;
			config.FontDataOwnedByAtlas = false;
			config.OversampleH = 2;
			config.OversampleV = 2;
			config.PixelSnapH = false;

			loaded_font = io.Fonts->AddFontFromMemoryTTF(
				g_font_data.data(),
				static_cast<int>(g_font_data.size()),
				k_custom_font_size,
				&config,
				io.Fonts->GetGlyphRangesDefault()
			);

			if (loaded_font)
			{
				g_active_font_path = requested_path;
			}
			else
			{
				g_font_error = "Unsupported or damaged font file: " +
					requested_path.string();
			}
		}

		if (!loaded_font)
		{
			loaded_font = io.Fonts->AddFontDefault();
		}

		io.FontDefault = loaded_font;
		io.Fonts->Build();
	}
}

namespace spintool
{
	SDL_Renderer* Renderer::s_renderer = nullptr;
	SDL_Window* Renderer::s_window = nullptr;
	SDLPaletteHandle Renderer::s_current_palette;
	std::recursive_mutex Renderer::s_sdl_update_mutex;

	void Renderer::SetPalette(const SDLPaletteHandle& palette)
	{
		if (!palette)
		{
			std::cerr << "Renderer::SetPalette received a null palette\n";
			return;
		}

		if (!s_current_palette || palette->ncolors != s_current_palette->ncolors)
		{
			s_current_palette = SDLPaletteHandle{ SDL_CreatePalette(palette->ncolors) };
			if (!s_current_palette)
			{
				std::cerr << "SDL_CreatePalette failed: " << SDL_GetError() << '\n';
				return;
			}
		}

		if (!SDL_SetPaletteColors(s_current_palette.get(), palette->colors, 0, palette->ncolors))
		{
			std::cerr << "SDL_SetPaletteColors failed: " << SDL_GetError() << '\n';
		}
	}

	SDLPaletteHandle Renderer::CreateSDLPalette(const rom::Palette& palette)
	{
		SDLPaletteHandle new_palette{ SDL_CreatePalette(16) };
		if (!new_palette)
		{
			std::cerr << "SDL_CreatePalette failed: " << SDL_GetError() << '\n';
			return {};
		}

		for (size_t i = 0; i < 16; ++i)
		{
			rom::Colour colour = palette.palette_swatches[i].GetUnpacked();
			new_palette->colors[i] = { colour.r, colour.g, colour.b, 255 };
		}
		return new_palette;
	}

	SDLPaletteHandle Renderer::CreateSDLPaletteForSet(const rom::PaletteSet& palette_set)
	{
		SDLPaletteHandle new_palette{ SDL_CreatePalette(64) };
		if (!new_palette)
		{
			std::cerr << "SDL_CreatePalette failed: " << SDL_GetError() << '\n';
			return {};
		}

		for (size_t a = 0; a < 4; ++a)
		{
			if (!palette_set.palette_lines[a])
			{
				std::cerr << "Palette set contains a null palette line\n";
				return {};
			}

			for (size_t i = 0; i < 16; ++i)
			{
				rom::Colour colour = palette_set.palette_lines[a]->palette_swatches[i].GetUnpacked();
				new_palette->colors[(a * 16) + i] = { colour.r, colour.g, colour.b, 255 };
			}
		}
		return new_palette;
	}

	bool Renderer::Initialise()
	{
		if (!SDL_CreateWindowAndRenderer(
			"SpinTool",
			s_window_width,
			s_window_height,
			SDL_WINDOW_RESIZABLE,
			&s_window,
			&s_renderer))
		{
			std::cerr << "SDL_CreateWindowAndRenderer failed: " << SDL_GetError() << '\n';
			return false;
		}

		if (!s_window || !s_renderer)
		{
			std::cerr << "SDL returned a null window or renderer\n";
			Shutdown();
			return false;
		}

		if (!SDL_MaximizeWindow(s_window))
		{
			std::cerr << "SDL_MaximizeWindow failed: " << SDL_GetError() << '\n';
		}

		if (!SDL_SetRenderDrawColor(s_renderer, 5, 10, 28, 255) ||
			!SDL_RenderClear(s_renderer))
		{
			std::cerr << "SDL renderer setup failed: " << SDL_GetError() << '\n';
			Shutdown();
			return false;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ApplyModernTheme();

		// The configurable font scale starts at 100%.
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->AddFontDefault();
		io.FontGlobalScale = 1.0f;

		if (!ImGui_ImplSDL3_InitForSDLRenderer(s_window, s_renderer))
		{
			std::cerr << "ImGui SDL3 backend initialisation failed\n";
			Shutdown();
			return false;
		}

		if (!ImGui_ImplSDLRenderer3_Init(s_renderer))
		{
			std::cerr << "ImGui SDL renderer backend initialisation failed\n";
			ImGui_ImplSDL3_Shutdown();
			Shutdown();
			return false;
		}

		return true;
	}

	void Renderer::ApplyModernTheme()
	{
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();

		style.WindowPadding = ImVec2(12.0f, 12.0f);
		style.FramePadding = ImVec2(9.0f, 6.0f);
		style.CellPadding = ImVec2(8.0f, 5.0f);
		style.ItemSpacing = ImVec2(9.0f, 7.0f);
		style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
		style.ScrollbarSize = 15.0f;
		style.GrabMinSize = 11.0f;

		style.WindowRounding = 9.0f;
		style.ChildRounding = 7.0f;
		style.FrameRounding = 6.0f;
		style.PopupRounding = 7.0f;
		style.ScrollbarRounding = 9.0f;
		style.GrabRounding = 6.0f;
		style.TabRounding = 6.0f;

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBorderSize = 0.0f;

		auto& colours = style.Colors;
		colours[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 1.00f, 1.00f);
		colours[ImGuiCol_TextDisabled] = ImVec4(0.53f, 0.57f, 0.70f, 1.00f);
		colours[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.040f, 0.090f, 0.965f);
		colours[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.050f, 0.115f, 0.940f);
		colours[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.055f, 0.125f, 0.985f);
		colours[ImGuiCol_Border] = ImVec4(0.34f, 0.25f, 0.72f, 0.72f);
		colours[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

		colours[ImGuiCol_FrameBg] = ImVec4(0.115f, 0.095f, 0.245f, 0.92f);
		colours[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.16f, 0.48f, 1.00f);
		colours[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.30f, 0.65f, 1.00f);

		colours[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.08f, 0.36f, 1.00f);
		colours[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.24f, 0.58f, 1.00f);
		colours[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.06f, 0.20f, 0.96f);
		colours[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.06f, 0.20f, 1.00f);

		colours[ImGuiCol_ScrollbarBg] = ImVec4(0.025f, 0.030f, 0.075f, 0.85f);
		colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.20f, 0.72f, 0.90f);
		colours[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.22f, 0.38f, 0.88f, 1.00f);
		colours[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.10f, 0.48f, 0.98f, 1.00f);

		colours[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
		colours[ImGuiCol_SliderGrab] = ImVec4(0.51f, 0.28f, 0.95f, 1.00f);
		colours[ImGuiCol_SliderGrabActive] = ImVec4(0.16f, 0.58f, 1.00f, 1.00f);

		colours[ImGuiCol_Button] = ImVec4(0.39f, 0.17f, 0.76f, 0.90f);
		colours[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.42f, 0.92f, 1.00f);
		colours[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.29f, 0.72f, 1.00f);

		colours[ImGuiCol_Header] = ImVec4(0.35f, 0.17f, 0.71f, 0.72f);
		colours[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.40f, 0.91f, 0.88f);
		colours[ImGuiCol_HeaderActive] = ImVec4(0.10f, 0.31f, 0.76f, 1.00f);

		colours[ImGuiCol_Separator] = ImVec4(0.34f, 0.25f, 0.72f, 0.66f);
		colours[ImGuiCol_SeparatorHovered] = ImVec4(0.22f, 0.52f, 1.00f, 0.90f);
		colours[ImGuiCol_SeparatorActive] = ImVec4(0.18f, 0.62f, 1.00f, 1.00f);
		colours[ImGuiCol_ResizeGrip] = ImVec4(0.42f, 0.22f, 0.86f, 0.35f);
		colours[ImGuiCol_ResizeGripHovered] = ImVec4(0.20f, 0.50f, 1.00f, 0.78f);
		colours[ImGuiCol_ResizeGripActive] = ImVec4(0.12f, 0.64f, 1.00f, 1.00f);

		colours[ImGuiCol_Tab] = ImVec4(0.15f, 0.09f, 0.32f, 0.96f);
		colours[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.43f, 0.92f, 1.00f);
		colours[ImGuiCol_TabActive] = ImVec4(0.35f, 0.17f, 0.73f, 1.00f);
		colours[ImGuiCol_TabUnfocused] = ImVec4(0.09f, 0.06f, 0.20f, 0.98f);
		colours[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.46f, 1.00f);

		colours[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.10f, 0.34f, 1.00f);
		colours[ImGuiCol_TableBorderStrong] = ImVec4(0.34f, 0.25f, 0.72f, 0.78f);
		colours[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.17f, 0.43f, 0.70f);
		colours[ImGuiCol_TableRowBg] = ImVec4(0.04f, 0.045f, 0.10f, 0.72f);
		colours[ImGuiCol_TableRowBgAlt] = ImVec4(0.075f, 0.065f, 0.16f, 0.72f);

		colours[ImGuiCol_TextSelectedBg] = ImVec4(0.24f, 0.39f, 0.92f, 0.48f);
		colours[ImGuiCol_NavHighlight] = ImVec4(0.22f, 0.55f, 1.00f, 1.00f);
		colours[ImGuiCol_ModalWindowDimBg] = ImVec4(0.015f, 0.010f, 0.055f, 0.76f);
	}

	void Renderer::RequestFont(const std::filesystem::path& font_path)
	{
		g_pending_font_path = font_path;
	}

	std::filesystem::path Renderer::GetActiveFontPath()
	{
		return g_active_font_path;
	}

	std::string Renderer::GetFontError()
	{
		return g_font_error;
	}

	void Renderer::Shutdown()
	{
		if (ImGui::GetCurrentContext())
		{
			ImGui_ImplSDLRenderer3_Shutdown();
			ImGui_ImplSDL3_Shutdown();
			ImGui::DestroyContext();
		}

		if (s_renderer)
		{
			SDL_DestroyRenderer(s_renderer);
			s_renderer = nullptr;
		}

		if (s_window)
		{
			SDL_DestroyWindow(s_window);
			s_window = nullptr;
		}
	}

	void Renderer::NewFrame()
	{
		if (!s_renderer || !s_window || !ImGui::GetCurrentContext())
		{
			return;
		}

		ApplyPendingFontRequest();
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
	}

	void Renderer::Render()
	{
		if (!s_renderer || !ImGui::GetCurrentContext())
		{
			return;
		}

		if (!SDL_RenderClear(s_renderer))
		{
			std::cerr << "SDL_RenderClear failed: " << SDL_GetError() << '\n';
			return;
		}

		ImGui::Render();
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);
		SDL_RenderPresent(s_renderer);
	}

	SDLTextureHandle Renderer::RenderArbitaryOffsetToTilesetTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions_in_tiles)
	{
		const Point tile_dimensions{ rom::TileSet::s_tile_width, rom::TileSet::s_tile_height, };
		const Point dimensions{ dimensions_in_tiles.x * tile_dimensions.x, dimensions_in_tiles.y * tile_dimensions.y };
		SDLSurfaceHandle new_surface{ SDL_CreateSurface(dimensions.x, dimensions.y, SDL_PIXELFORMAT_RGBA32) };
		SDLSurfaceHandle tile_surface{ SDL_CreateSurface(rom::TileSet::s_tile_width, rom::TileSet::s_tile_height, SDL_PIXELFORMAT_RGBA32) };
		if (!new_surface || !tile_surface)
		{
			std::cerr << "SDL_CreateSurface failed: " << SDL_GetError() << '\n';
			return {};
		}
		Uint32 next_offset = offset;
		for (int i = 0; i < dimensions_in_tiles.x * dimensions_in_tiles.y; ++i)
		{
			SDL_ClearSurface(tile_surface.get(), 0.0f, 0.0f, 0.0f, 0.0f);
			rom.RenderToSurface(tile_surface.get(), offset, tile_dimensions);

			SDL_Rect target_rect{ (i % dimensions_in_tiles.x) * tile_surface->w, ((i - (i % dimensions_in_tiles.x)) / dimensions_in_tiles.x) * tile_surface->h, tile_surface->w, tile_surface->h};

			if (!SDL_BlitSurface(tile_surface.get(), nullptr, new_surface.get(), &target_rect))
			{
				std::cerr << "SDL_BlitSurface failed: " << SDL_GetError() << '\n';
				return {};
			}
			offset += static_cast<Uint32>(tile_surface->w * tile_surface->h * 2);
		}
		return RenderToTexture(new_surface.get());
	}

	SDLTextureHandle Renderer::RenderArbitaryOffsetToTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions)
	{
		SDLSurfaceHandle new_surface{ SDL_CreateSurface(dimensions.x, dimensions.y, SDL_PIXELFORMAT_RGBA32) };
		if (!new_surface)
		{
			std::cerr << "SDL_CreateSurface failed: " << SDL_GetError() << '\n';
			return {};
		}
		rom.RenderToSurface(new_surface.get(), offset, dimensions);
		return RenderToTexture(new_surface.get());
	}

	SDLTextureHandle Renderer::RenderArbitaryOffsetToTexture(const rom::SpinballROM& rom, Uint32 offset, Point dimensions, const rom::Palette& palette)
	{
		SDLSurfaceHandle new_surface{ SDL_CreateSurface(dimensions.x, dimensions.y, SDL_PIXELFORMAT_RGBA32) };
		if (!new_surface)
		{
			std::cerr << "SDL_CreateSurface failed: " << SDL_GetError() << '\n';
			return {};
		}
		//UIPalette ui_palette{palette};
		//SDL_SetSurfacePalette(new_surface.get(), ui_palette.sdl_palette.get());
		rom.RenderToSurface(new_surface.get(), offset, dimensions, palette);
		return RenderToTexture(new_surface.get());
	}

	SDLTextureHandle Renderer::RenderToTexture(const rom::Sprite& sprite, bool flip_x, bool flip_y)
	{
		if (!s_current_palette)
		{
			std::cerr << "Cannot render sprite: no palette has been selected\n";
			return {};
		}

		SDLSurfaceHandle sprite_atlas_surface{ SDL_CreateSurface(
			sprite.GetBoundingBox().Width(),
			sprite.GetBoundingBox().Height(),
			SDL_PIXELFORMAT_INDEX8) };

		if (!sprite_atlas_surface)
		{
			std::cerr << "SDL_CreateSurface failed: " << SDL_GetError() << '\n';
			return {};
		}

		if (!SDL_SetSurfacePalette(sprite_atlas_surface.get(), s_current_palette.get()) ||
			!SDL_SetSurfaceColorKey(sprite_atlas_surface.get(), true, 0))
		{
			std::cerr << "Sprite surface setup failed: " << SDL_GetError() << '\n';
			return {};
		}

		sprite.RenderToSurface(sprite_atlas_surface.get());
		if (flip_x && !SDL_FlipSurface(sprite_atlas_surface.get(), SDL_FLIP_HORIZONTAL))
		{
			std::cerr << "SDL_FlipSurface failed: " << SDL_GetError() << '\n';
			return {};
		}

		if (flip_y && !SDL_FlipSurface(sprite_atlas_surface.get(), SDL_FLIP_VERTICAL))
		{
			std::cerr << "SDL_FlipSurface failed: " << SDL_GetError() << '\n';
			return {};
		}

		return RenderToTexture(sprite_atlas_surface.get());
	}

	SDLTextureHandle Renderer::RenderToTexture(const rom::SpriteTile& sprite_tile)
	{
		if (!s_current_palette)
		{
			std::cerr << "Cannot render sprite tile: no palette has been selected\n";
			return {};
		}

		SDLSurfaceHandle sprite_atlas_surface{
			SDL_CreateSurface(sprite_tile.x_size, sprite_tile.y_size, SDL_PIXELFORMAT_INDEX8)
		};
		if (!sprite_atlas_surface)
		{
			std::cerr << "SDL_CreateSurface failed: " << SDL_GetError() << '\n';
			return {};
		}

		if (!SDL_SetSurfacePalette(sprite_atlas_surface.get(), s_current_palette.get()))
		{
			std::cerr << "SDL_SetSurfacePalette failed: " << SDL_GetError() << '\n';
			return {};
		}

		sprite_tile.RenderToSurface(sprite_atlas_surface.get());
		return RenderToTexture(sprite_atlas_surface.get());
	}

	SDLTextureHandle Renderer::RenderToTexture(SDL_Surface* surface)
	{
		if (!s_renderer || !surface)
		{
			std::cerr << "Cannot create texture: null renderer or surface\n";
			return {};
		}

		SDLTextureHandle new_texture{ SDL_CreateTextureFromSurface(s_renderer, surface) };
		if (!new_texture)
		{
			std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << '\n';
			return {};
		}

		if (!SDL_SetTextureScaleMode(new_texture.get(), SDL_SCALEMODE_NEAREST))
		{
			std::cerr << "SDL_SetTextureScaleMode failed: " << SDL_GetError() << '\n';
			return {};
		}
		return new_texture;
	}

}
