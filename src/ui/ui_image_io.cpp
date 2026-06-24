#include "ui/ui_image_io.h"

#include "SDL3/SDL_error.h"
#include "SDL3/SDL_iostream.h"
#include "SDL3/SDL_image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace spintool
{
	std::string PathToUtf8(const std::filesystem::path& path)
	{
#if defined(__cpp_lib_char8_t)
		const std::u8string utf8_path = path.u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8_path.data()),
			utf8_path.size()
		);
#else
		return path.u8string();
#endif
	}

	SDLSurfaceHandle LoadImageFromPath(
		const std::filesystem::path& path,
		std::string* error_message
	)
	{
		auto set_error = [error_message](const std::string& message)
		{
			if (error_message != nullptr)
			{
				*error_message = message;
			}
		};

		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			set_error("Could not open the selected image file.");
			return {};
		}

		input.seekg(0, std::ios::end);
		const std::streamoff file_size = input.tellg();
		if (file_size <= 0)
		{
			set_error("The selected image file is empty.");
			return {};
		}
		if (static_cast<std::uintmax_t>(file_size) >
			static_cast<std::uintmax_t>(std::numeric_limits<int>::max()))
		{
			set_error("The selected image file is too large.");
			return {};
		}

		input.seekg(0, std::ios::beg);
		std::vector<Uint8> file_bytes(static_cast<std::size_t>(file_size));
		if (!input.read(
			reinterpret_cast<char*>(file_bytes.data()),
			file_size
		))
		{
			set_error("Could not read the selected image file.");
			return {};
		}

		constexpr std::array<Uint8, 8> png_signature{
			0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU
		};
		if (file_bytes.size() < png_signature.size() ||
			!std::equal(
				png_signature.begin(),
				png_signature.end(),
				file_bytes.begin()
			))
		{
			set_error("The selected file is not a valid PNG file.");
			return {};
		}

		SDL_IOStream* stream = SDL_IOFromConstMem(
			file_bytes.data(),
			file_bytes.size()
		);
		if (stream == nullptr)
		{
			set_error(std::string("Could not create an image stream: ") + SDL_GetError());
			return {};
		}

		// The Windows package uses SDL3_image built by vcpkg. Pass the explicit
		// type so SDL_image does not have to infer it from an in-memory stream.
		SDL_ClearError();
		SDLSurfaceHandle surface{IMG_LoadTyped_IO(stream, true, "PNG")};
		if (!surface)
		{
			set_error(
				std::string("SDL_image could not decode the PNG: ") +
				SDL_GetError() +
				". On Windows, make sure the package was built with "
				"the SDL3_image PNG feature and includes its dependency DLLs."
			);
			return {};
		}

		if (error_message != nullptr)
		{
			error_message->clear();
		}
		return surface;
	}
}
