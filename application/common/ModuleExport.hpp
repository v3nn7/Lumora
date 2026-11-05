#pragma once

// Windows import/export of classes whose *data* other modules use - Qt's staticMetaObject of
// QObject classes, used by connect() and qobject_cast across modules. Functions are exported
// by CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS; on other platforms and in static builds these are empty.
// CMake defines lumora_<module>_EXPORTS while building that module's shared library.

#if defined(_WIN32) && defined(LUMORA_SHARED_MODULES)
#define LUMORA_DLL_EXPORT __declspec(dllexport)
#define LUMORA_DLL_IMPORT __declspec(dllimport)
#else
#define LUMORA_DLL_EXPORT
#define LUMORA_DLL_IMPORT
#endif

#if defined(lumora_appcommon_EXPORTS)
#define LUMORA_APPCOMMON_API LUMORA_DLL_EXPORT
#else
#define LUMORA_APPCOMMON_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_collab_EXPORTS)
#define LUMORA_COLLAB_API LUMORA_DLL_EXPORT
#else
#define LUMORA_COLLAB_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_launcher_EXPORTS)
#define LUMORA_LAUNCHER_API LUMORA_DLL_EXPORT
#else
#define LUMORA_LAUNCHER_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_hub_EXPORTS)
#define LUMORA_HUB_API LUMORA_DLL_EXPORT
#else
#define LUMORA_HUB_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_editor_EXPORTS)
#define LUMORA_EDITOR_API LUMORA_DLL_EXPORT
#else
#define LUMORA_EDITOR_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_player_EXPORTS)
#define LUMORA_PLAYER_API LUMORA_DLL_EXPORT
#else
#define LUMORA_PLAYER_API LUMORA_DLL_IMPORT
#endif

#if defined(lumora_shell_EXPORTS)
#define LUMORA_SHELL_API LUMORA_DLL_EXPORT
#else
#define LUMORA_SHELL_API LUMORA_DLL_IMPORT
#endif
