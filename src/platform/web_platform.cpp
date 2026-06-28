#include "platform/web_platform.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

namespace spintool::web
{
#if defined(__EMSCRIPTEN__)
    namespace
    {
        struct FileRequestState
        {
            bool finished = false;
            std::optional<std::filesystem::path> selected_path;
        };

        std::map<int, FileRequestState> g_file_requests;
        int g_next_request_id = 1;

        std::string BuildAcceptFilter(const std::vector<std::string>& extensions)
        {
            std::ostringstream accept;
            bool first = true;
            for (std::string extension : extensions)
            {
                if (extension.empty())
                {
                    continue;
                }
                if (extension.front() != '.')
                {
                    extension.insert(extension.begin(), '.');
                }
                std::transform(
                    extension.begin(),
                    extension.end(),
                    extension.begin(),
                    [](unsigned char value)
                    {
                        return static_cast<char>(std::tolower(value));
                    }
                );
                if (!first)
                {
                    accept << ',';
                }
                accept << extension;
                first = false;
            }
            return accept.str();
        }
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void spintool_web_file_selected(
        int request_id,
        const char* virtual_path
    )
    {
        auto request = g_file_requests.find(request_id);
        if (request == g_file_requests.end())
        {
            return;
        }

        request->second.finished = true;
        if (virtual_path != nullptr && virtual_path[0] != '\0')
        {
            request->second.selected_path = std::filesystem::path{virtual_path};
        }
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void spintool_web_file_cancelled(int request_id)
    {
        auto request = g_file_requests.find(request_id);
        if (request != g_file_requests.end())
        {
            request->second.finished = true;
            request->second.selected_path.reset();
        }
    }

    EM_JS(void, OpenBrowserFilePicker, (
        int request_id,
        const char* accept_utf8,
        const char* target_directory_utf8
    ), {
        const accept = UTF8ToString(accept_utf8);
        const targetDirectory = UTF8ToString(target_directory_utf8);
        const input = document.createElement('input');
        input.type = 'file';
        input.accept = accept;
        input.style.display = 'none';

        let completed = false;
        let selectionStarted = false;
        const cancel = () => {
            const pickerPrompt = document.getElementById('file-picker-prompt');
            if (pickerPrompt) pickerPrompt.classList.add('hidden');
            if (!completed) {
                completed = true;
                _spintool_web_file_cancelled(request_id);
            }
            input.remove();
        };

        input.addEventListener('cancel', cancel, { once: true });
        input.addEventListener('change', async () => {
            selectionStarted = true;
            const file = input.files && input.files.length > 0 ? input.files[0] : null;
            if (!file) {
                cancel();
                return;
            }

            try {
                let importedFile = file;
                let safeName = file.name.replace(/[\\/\0]/g, '_');

                // SDL3's lightweight web build uses its native PNG loader.
                // Convert other browser-supported image formats to PNG before
                // exposing them to the unchanged C++ import code.
                const lowerName = safeName.toLowerCase();
                const hasImageExtension = /\.(png|gif|bmp|jpe?g|webp)$/i.test(safeName);
                const isBrowserImage = file.type.startsWith('image/') || hasImageExtension;
                if (isBrowserImage && !lowerName.endsWith('.png')) {
                    const bitmap = await createImageBitmap(file);
                    let pngBlob;
                    if (typeof OffscreenCanvas !== 'undefined') {
                        const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
                        const context = canvas.getContext('2d', { alpha: true });
                        context.drawImage(bitmap, 0, 0);
                        pngBlob = await canvas.convertToBlob({ type: 'image/png' });
                    } else {
                        const canvas = document.createElement('canvas');
                        canvas.width = bitmap.width;
                        canvas.height = bitmap.height;
                        canvas.getContext('2d', { alpha: true }).drawImage(bitmap, 0, 0);
                        pngBlob = await new Promise((resolve, reject) => {
                            canvas.toBlob(
                                (blob) => blob ? resolve(blob) : reject(new Error('PNG conversion failed')),
                                'image/png'
                            );
                        });
                    }
                    bitmap.close();
                    importedFile = pngBlob;
                    safeName = safeName.replace(/\.[^.]*$/, '') + '.png';
                }

                const bytes = new Uint8Array(await importedFile.arrayBuffer());
                FS.mkdirTree(targetDirectory);
                const virtualPath = `${targetDirectory}/${safeName}`;
                FS.writeFile(virtualPath, bytes);

                completed = true;
                const pathLength = lengthBytesUTF8(virtualPath) + 1;
                const pathPointer = _malloc(pathLength);
                stringToUTF8(virtualPath, pathPointer, pathLength);
                _spintool_web_file_selected(request_id, pathPointer);
                _free(pathPointer);

                if (typeof FS.syncfs === 'function') {
                    FS.syncfs(false, (error) => {
                        if (error) {
                            console.error('SpinTool: IndexedDB sync failed after import', error);
                        }
                    });
                }
            } catch (error) {
                console.error('SpinTool: could not import selected file', error);
                cancel();
            } finally {
                input.remove();
            }
        }, { once: true });

        const prompt = document.getElementById('file-picker-prompt');
        const promptText = document.getElementById('file-picker-prompt-text');
        const chooseButton = document.getElementById('file-picker-choose');
        const cancelButton = document.getElementById('file-picker-cancel');

        const hidePrompt = () => {
            if (prompt) prompt.classList.add('hidden');
        };

        const showPrompt = () => {
            if (!prompt || !chooseButton || !cancelButton) {
                return false;
            }
            if (promptText) {
                promptText.textContent = accept
                    ? `Choose a file (${accept}) to continue.`
                    : 'Choose a file to continue.';
            }
            prompt.classList.remove('hidden');
            chooseButton.onclick = () => {
                hidePrompt();
                launchPicker();
            };
            cancelButton.onclick = () => {
                hidePrompt();
                cancel();
            };
            return true;
        };

        // Some browsers do not emit the input `cancel` event. When the
        // picker returns focus without starting a selection, complete the C++
        // request as cancelled so a later click can open a new picker.
        const onWindowFocus = () => {
            window.setTimeout(() => {
                if (!completed && !selectionStarted) {
                    cancel();
                }
            }, 350);
        };

        const launchPicker = () => {
            if (!input.isConnected) document.body.appendChild(input);
            window.addEventListener('focus', onWindowFocus, { once: true });
            input.click();

            // If a browser rejects the programmatic click because the SDL
            // frame no longer owns transient user activation, offer one real
            // HTML button click as a portable fallback.
            window.setTimeout(() => {
                if (!completed && !selectionStarted && document.hasFocus()) {
                    showPrompt();
                }
            }, 700);
        };

        if (navigator.userActivation && !navigator.userActivation.isActive) {
            if (!showPrompt()) launchPicker();
        } else {
            launchPicker();
        }
    });

    EM_JS(void, SyncBrowserFilesystem, (), {
        if (typeof FS !== 'undefined' && typeof FS.syncfs === 'function') {
            FS.syncfs(false, (error) => {
                if (error) {
                    console.error('SpinTool: IndexedDB sync failed', error);
                }
            });
        }
    });

    EM_JS(int, DownloadBrowserFile, (
        const char* path_utf8,
        const char* mime_type_utf8,
        const char* download_name_utf8
    ), {
        try {
            const path = UTF8ToString(path_utf8);
            const mimeType = UTF8ToString(mime_type_utf8);
            const requestedName = UTF8ToString(download_name_utf8);
            const bytes = FS.readFile(path, { encoding: 'binary' });
            const blob = new Blob([bytes], { type: mimeType || 'application/octet-stream' });
            const anchor = document.createElement('a');
            anchor.href = URL.createObjectURL(blob);
            anchor.download = requestedName || path.split('/').pop() || 'spintool-export.bin';
            anchor.style.display = 'none';
            document.body.appendChild(anchor);
            anchor.click();
            window.setTimeout(() => {
                URL.revokeObjectURL(anchor.href);
                anchor.remove();
            }, 1000);
            return 1;
        } catch (error) {
            console.error('SpinTool: download failed', error);
            return 0;
        }
    });
#endif

    bool IsWebBuild()
    {
#if defined(__EMSCRIPTEN__)
        return true;
#else
        return false;
#endif
    }

    int RequestFile(
        const std::vector<std::string>& extensions,
        const std::filesystem::path& target_directory
    )
    {
#if defined(__EMSCRIPTEN__)
        const int request_id = g_next_request_id++;
        g_file_requests.emplace(request_id, FileRequestState{});
        const std::string accept = BuildAcceptFilter(extensions);
        const std::string target = target_directory.generic_string();
        OpenBrowserFilePicker(request_id, accept.c_str(), target.c_str());
        return request_id;
#else
        (void)extensions;
        (void)target_directory;
        return 0;
#endif
    }

    std::optional<std::filesystem::path> PollSelectedFile(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        const auto request = g_file_requests.find(request_id);
        if (request == g_file_requests.end() || !request->second.finished)
        {
            return std::nullopt;
        }
        return request->second.selected_path;
#else
        (void)request_id;
        return std::nullopt;
#endif
    }

    bool FileRequestFinished(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        const auto request = g_file_requests.find(request_id);
        return request != g_file_requests.end() && request->second.finished;
#else
        (void)request_id;
        return true;
#endif
    }

    void ForgetFileRequest(int request_id)
    {
#if defined(__EMSCRIPTEN__)
        g_file_requests.erase(request_id);
#else
        (void)request_id;
#endif
    }

    void SyncPersistentStorage()
    {
#if defined(__EMSCRIPTEN__)
        SyncBrowserFilesystem();
#endif
    }

    bool DownloadFile(
        const std::filesystem::path& path,
        const std::string& mime_type,
        const std::string& download_name
    )
    {
#if defined(__EMSCRIPTEN__)
        const std::string virtual_path = path.generic_string();
        const std::string filename = download_name.empty()
            ? path.filename().string()
            : download_name;
        return DownloadBrowserFile(
            virtual_path.c_str(),
            mime_type.c_str(),
            filename.c_str()
        ) != 0;
#else
        (void)path;
        (void)mime_type;
        (void)download_name;
        return false;
#endif
    }
}
