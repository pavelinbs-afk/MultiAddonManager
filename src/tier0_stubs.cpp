/**
 * CS2 engine updates have removed/changed some tier0 exports that HL2SDK
 * memoverride and related headers still reference. Provide local stubs so the
 * plugin can be loaded (dlopen) on current dedicated servers.
 */
extern "C" bool g_bUpdateStringTokenDatabase = false;
