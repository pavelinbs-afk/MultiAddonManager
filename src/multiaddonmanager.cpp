/**
 * =============================================================================
 * MultiAddonManager
 * Copyright (C) 2024-2025 xen
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "networkbasetypes.pb.h"
#include "gameevents.pb.h"

#include <stdio.h>
#include "multiaddonmanager.h"
#include "module.h"
#include "utils/plat.h"
#include "networksystem/inetworkserializer.h"
#include "networksystem/inetworkmessages.h"
#include "convar.h"
#include "hoststate.h"
#include "igameeventsystem.h"
#include "serversideclient.h"
#include "funchook.h"
#include "filesystem.h"
#include "steam/steam_gameserver.h"
#include <string>
#include <sstream>
#include <fstream>
#include "iserver.h"

#include "tier0/memdbgon.h"

CConVar<bool> mm_addon_mount_download("mm_addon_mount_download", FCVAR_NONE, "Whether to download an addon upon mounting even if it's installed", false);
CConVar<bool> mm_block_disconnect_messages("mm_block_disconnect_messages", FCVAR_NONE, "Whether to block \"loop shutdown\" disconnect messages", false);
CConVar<bool> mm_cache_clients_with_addons("mm_cache_clients_with_addons", FCVAR_NONE, "Whether to cache clients addon download list, this will prevent reconnects on mapchange/rejoin", false);
CConVar<float> mm_cache_clients_duration("mm_cache_clients_duration", FCVAR_NONE, "How long to cache clients' downloaded addons list in seconds, pass 0 for forever.", 0.0f);
CConVar<float> mm_addon_connection_timeout("mm_addon_connection_timeout", FCVAR_NONE, "How long until clients are timed out while downloading the first required addon (usually the current map), 0 disables", 30.f);
CConVar<float> mm_extra_addons_timeout("mm_extra_addons_timeout", FCVAR_NONE, "How long until clients are timed out in between connects for extra addons in seconds, requires mm_extra_addons to be used", 10.f);
CConVar<float> mm_steam_refresh_delay("mm_steam_refresh_delay", FCVAR_NONE, "Minimum seconds to wait after Steam API activation before refreshing/downloading addons. Also waits until the gameserver is logged on to Steam.", 5.f);

CConVar<bool> mm_addon_debug("mm_addon_debug", FCVAR_NONE, "Whether to print some extra debug information", false);

void Message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	LoggingSystem_Log(2, LS_MESSAGE, Color(0, 255, 200), "[MultiAddonManager] %s", buf);

	va_end(args);
}

void Panic(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	Warning("[MultiAddonManager] %s", buf);

	va_end(args);
}

void StringToVector(const char *pszString, CUtlVector<std::string> &vector)
{
	std::stringstream stream(pszString);

	vector.RemoveAll();

	while (stream.good())
	{
		std::string substr;
		getline(stream, substr, ',');

		if (!substr.empty())
			vector.AddToTail(substr);
	}
}

std::string VectorToString(CUtlVector<std::string> &vector)
{
	std::string result;

	FOR_EACH_VEC(vector, i)
	{
		result += vector[i];

		if (i + 1 < vector.Count())
			result += ',';
	}

	return result;
}

ISteamUGC *GetSteamUGC()
{
	if (g_pEngineServer->IsDedicatedServer())
		return SteamGameServerUGC();
	else
		return SteamUGC();
}

typedef bool (FASTCALL *SendNetMessage_t)(CServerSideClientBase *, CNetMessage*, NetChannelBufType_t);
typedef void (FASTCALL *HostStateRequest_t)(CHostStateMgr*, CHostStateRequest*);
typedef void (FASTCALL *ReplyConnection_t)(INetworkGameServer *, CServerSideClient *);
typedef uint64 (FASTCALL *ScriptGetAddon_t)();

bool FASTCALL Hook_SendNetMessage_ServerSideClient(CServerSideClientBase *pClient, CNetMessage *pData, NetChannelBufType_t bufType);
bool FASTCALL Hook_SendNetMessage_HLTVClient(CServerSideClientBase *pClient, CNetMessage *pData, NetChannelBufType_t bufType);
void FASTCALL Hook_SetPendingHostStateRequest(CHostStateMgr*, CHostStateRequest*);
void FASTCALL Hook_ReplyConnection(INetworkGameServer *, CServerSideClient *);
uint64 FASTCALL Hook_ScriptGetAddon();

SendNetMessage_t g_pfnSendNetMessage_ServerSideClient = nullptr;
SendNetMessage_t g_pfnSendNetMessage_HLTVClient = nullptr;
HostStateRequest_t g_pfnSetPendingHostStateRequest = nullptr;
ReplyConnection_t g_pfnReplyConnection = nullptr;
ScriptGetAddon_t g_pfnScriptGetAddon = nullptr;

funchook_t *g_pSendNetMessageHook_ServerSideClient = nullptr;
funchook_t *g_pSendNetMessageHook_HLTVClient = nullptr;
funchook_t *g_pSetPendingHostStateRequest = nullptr;
funchook_t *g_pReplyConnectionHook = nullptr;
funchook_t *g_pScriptGetAddonHook = nullptr;

int g_iLoadEventsFromFileHookId = -1;
static void* g_pEventMgrVtbl = nullptr;

class GameSessionConfiguration_t { };

using PostEventClientsFn = void (IGameEventSystem::*)(CSplitScreenSlot, bool, int, const uint64 *,
	INetworkMessageInternal *, const CNetMessage *, unsigned long, NetChannelBufType_t);

template <typename CLASS, typename RETURN, typename... ARGS>
static void AddGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	struct { void* v; } fake{ vtbl };
	hook.AddGlobal(reinterpret_cast<CLASS*>(&fake));
}

template <typename CLASS, typename RETURN, typename... ARGS>
static void RemoveGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	if (!vtbl)
		return;
	struct { void* v; } fake{ vtbl };
	hook.RemoveGlobal(reinterpret_cast<CLASS*>(&fake));
}

MultiAddonManager::MultiAddonManager() :
	m_GameServerSteamAPIActivated(&ISource2Server::GameServerSteamAPIActivated, this, &MultiAddonManager::Hook_GameServerSteamAPIActivated, nullptr),
	m_StartupServer(&INetworkServerService::StartupServer, this, nullptr, &MultiAddonManager::Hook_StartupServer),
	m_ClientConnect(&IServerGameClients::ClientConnect, this, &MultiAddonManager::Hook_ClientConnect, nullptr),
	m_ClientDisconnect(&IServerGameClients::ClientDisconnect, this, nullptr, &MultiAddonManager::Hook_ClientDisconnect),
	m_ClientActive(&IServerGameClients::ClientActive, this, nullptr, &MultiAddonManager::Hook_ClientActive),
	m_GameFrame(&ISource2Server::GameFrame, this, nullptr, &MultiAddonManager::Hook_GameFrame),
	m_PostEvent(static_cast<PostEventClientsFn>(&IGameEventSystem::PostEventAbstract), this, &MultiAddonManager::Hook_PostEvent, nullptr),
	m_LoadEventsFromFile(&IGameEventManager2::LoadEventsFromFile, this, &MultiAddonManager::Hook_LoadEventsFromFile, nullptr),
	m_CanHLTVClientConnect(&IServerGameClients::CanHLTVClientConnect, this, &MultiAddonManager::Hook_CanHLTVClientConnect, nullptr)
{
}

// Signatures

// "Discarding pending request '%s, %u'\n"
// "Sending S2C_CONNECTION to %s [addons:'%s']\n"
// First call without args in func with "Saving existing workshop save file from %s\n"
#ifdef PLATFORM_WINDOWS
constexpr const byte g_HostStateRequest_Sig[] = "\x48\x89\x74\x24\x2A\x57\x48\x83\xEC\x2A\x33\xF6\x48\x8B\xFA\x48\x39\x35";
constexpr const byte g_ReplyConnection_Sig[] = "\x48\x8B\xC4\x55\x41\x55\x41\x56";
constexpr const byte g_ScriptGetAddon_Sig[] = "\x40\x56\x48\x83\xEC\x2A\x48\x8B\x0D\x2A\x2A\x2A\x2A\x48\x8D\x54\x24";
#else
constexpr const byte g_HostStateRequest_Sig[] = "\x55\x48\x89\xE5\x41\x56\x41\x55\x41\x54\x49\x89\xF4\x53\x48\x83\x7F";
// Extended upstream (#79): "55 B9 ? ? ? ? 41 B8 ? ? ? ? 48 89 E5"
constexpr const byte g_ReplyConnection_Sig[] = "\x55\xB9\x2A\x2A\x2A\x2A\x41\xB8\x2A\x2A\x2A\x2A\x48\x89\xE5";
constexpr const byte g_ScriptGetAddon_Sig[] = "\x55\x48\x89\xE5\x41\x55\x41\x54\x48\x8D\x75\x2A\x53\x48\x83\xEC\x2A\x48\x8D\x05\x2A\x2A\x2A\x2A\x48\xC7\x45\x2A\x2A\x2A\x2A\x2A\x48\xC7\x45\x2A\x2A\x2A\x2A\x2A\x48\x8B\x38\x48\x8B\x07\xFF\x90\x2A\x2A\x2A\x2A\x8B\x55";
#endif


// Offsets
constexpr int g_iServerAddonsOffset = 344;
constexpr int g_iClientListOffset = 584;

#ifdef PLATFORM_WINDOWS
constexpr int g_iSendNetMessageOffset = 15;
#else
constexpr int g_iSendNetMessageOffset = 16;
#endif

/* 
The general workflow is defined as follows:
0. The server defines a list of server side addons and global client side addons to mount.
1. Client connects and request for the list of addons through ReplyConnection. MAM get the full list of addons to load.
2. If there is at least one addon to load, client will be prompted to download the first addon.
3. Once done, client reconnects and ClientConnect fires. If connected within the timeout interval, MAM marks this first addon as downloaded, 
	then check if there are other addons to download. If there is at least one, it will send a signon message to the client through SendNetMessage.
4. Client will be prompted to download the next addon. Client reconnects and ClientConnect fires again. 
	MAM marks the previous downloading addon (the one sent in the previous signon message) as done, and keep sending signon messages until all addons are downloaded.
5. Once all addons are downloaded, MAM stops sending custom signon messages.
Note: 
The list of addons to download does not have to be in order, but the addon list that the client uses to load is. This order is somewhat arbitrarily defined as follows: 
	- Original server workshop map (if any)
	- Server side *mounted* addons (m_MountedAddons)
	- Client side global addons (m_GlobalClientAddons)
	- Client side client-specific addons (addonsToLoad).
While plugins using the interface can add/remove addons at any time between these steps, it should be fine since the list of addon to load is newly checked every time the client connects.
*/

enum ClientConnectedState_t
{
	CLIENTCONN_NONE,
	CLIENTCONN_CONNECTING,
	CLIENTCONN_JOINED
};

struct ClientAddonInfo_t
{
	double lastActiveTime {};
	CUtlVector<std::string> addonsToLoad;
	CUtlVector<std::string> downloadedAddons;
	std::string currentPendingAddon;
	ClientConnectedState_t connectedState = CLIENTCONN_NONE;
	double connectionStartTime {};
};

std::unordered_map<uint64, ClientAddonInfo_t> g_ClientAddons;

CUtlVector<CServerSideClient *> *GetClientList()
{
	if (!g_pNetworkServerService)
		return nullptr;

	return (CUtlVector<CServerSideClient *> *)((char *)g_pNetworkServerService->GetIGameServer() + g_iClientListOffset);
}

static CServerSideClient *FindClientBySteamID(uint64 steamID64)
{
	CUtlVector<CServerSideClient *> *clients = GetClientList();
	if (!clients)
		return nullptr;

	CUtlVector<CServerSideClient *> &clientList = *clients;
	FOR_EACH_VEC(clientList, i)
	{
		CServerSideClient *client = clientList[i];
		if (client && client->GetClientSteamID().ConvertToUint64() == steamID64)
			return client;
	}

	return nullptr;
}

CConVar<CUtlString> mm_extra_addons("mm_extra_addons", FCVAR_NONE, "The workshop IDs of extra addons separated by commas, addons will be downloaded (if not present) and mounted", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_ExtraAddons);

		g_MultiAddonManager.RequestRefreshAddons();
	});


CConVar<CUtlString> mm_client_extra_addons("mm_client_extra_addons", FCVAR_NONE, "The workshop IDs of extra client addons that will be applied to all clients, separated by commas", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_GlobalClientAddons);
	});

MultiAddonManager g_MultiAddonManager;
INetworkGameServer *g_pNetworkGameServer = nullptr;
CGlobalVars *gpGlobals = nullptr;
IGameEventSystem *g_pGameEventSystem = nullptr;
IGameEventManager2 *g_pGameEventManager = nullptr;

PLUGIN_EXPOSE(MultiAddonManager, g_MultiAddonManager);
bool MultiAddonManager::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	// Required to get the IMetamodListener events
	g_SMAPI->AddListener( this, this );

	CModule engineModule(ROOTBIN, "engine2");
	CModule serverModule(GAMEBIN, "server");

	int sig_error;

	g_pfnSetPendingHostStateRequest = (HostStateRequest_t)engineModule.FindSignature(g_HostStateRequest_Sig, sizeof(g_HostStateRequest_Sig) - 1, sig_error);

	if (!g_pfnSetPendingHostStateRequest)
	{
		V_snprintf(error, maxlen, "Could not find the signature for HostStateRequest\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for HostStateRequest occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pSetPendingHostStateRequest = funchook_create();
	funchook_prepare(g_pSetPendingHostStateRequest, (void**)&g_pfnSetPendingHostStateRequest, (void*)Hook_SetPendingHostStateRequest);
	funchook_install(g_pSetPendingHostStateRequest, 0);

	// We're using funchook even though it's a virtual function because it can be called on a different thread and KHook isn't thread-safe
	void **pServerSideClientVTable = (void **)engineModule.FindVirtualTable("CServerSideClient");
	g_pfnSendNetMessage_ServerSideClient = (SendNetMessage_t)pServerSideClientVTable[g_iSendNetMessageOffset];

	g_pSendNetMessageHook_ServerSideClient = funchook_create();
	funchook_prepare(g_pSendNetMessageHook_ServerSideClient, (void**)&g_pfnSendNetMessage_ServerSideClient, (void*)Hook_SendNetMessage_ServerSideClient);
	funchook_install(g_pSendNetMessageHook_ServerSideClient, 0);

	void **pHLTVClientVTable = (void **)engineModule.FindVirtualTable("CHLTVClient");
	g_pfnSendNetMessage_HLTVClient = (SendNetMessage_t)pHLTVClientVTable[g_iSendNetMessageOffset];

	g_pSendNetMessageHook_HLTVClient = funchook_create();
	funchook_prepare(g_pSendNetMessageHook_HLTVClient, (void **)&g_pfnSendNetMessage_HLTVClient, (void *)Hook_SendNetMessage_HLTVClient);
	funchook_install(g_pSendNetMessageHook_HLTVClient, 0);

	g_pfnReplyConnection = (ReplyConnection_t)engineModule.FindSignature(g_ReplyConnection_Sig, sizeof(g_ReplyConnection_Sig) - 1, sig_error);

	if (!g_pfnReplyConnection)
	{
		V_snprintf(error, maxlen, "Could not find the signature for ReplyConnection\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for ReplyConnection occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pReplyConnectionHook = funchook_create();
	funchook_prepare(g_pReplyConnectionHook, (void**)&g_pfnReplyConnection, (void*)Hook_ReplyConnection);
	funchook_install(g_pReplyConnectionHook, 0);
	
	g_pfnScriptGetAddon = (ScriptGetAddon_t)serverModule.FindSignature(g_ScriptGetAddon_Sig, sizeof(g_ScriptGetAddon_Sig) - 1, sig_error);

	if (!g_pfnScriptGetAddon)
	{
		V_snprintf(error, maxlen, "Could not find the signature for ScriptGetAddon\n");
		Panic("%s", error);
		return false;
	}
	else if (sig_error == SIG_FOUND_MULTIPLE)
	{
		Panic("Signature for ScriptGetAddon occurs multiple times! Using first match but this might end up crashing!\n");
	}

	g_pScriptGetAddonHook = funchook_create();
	funchook_prepare(g_pScriptGetAddonHook, (void**)&g_pfnScriptGetAddon, (void*)Hook_ScriptGetAddon);
	funchook_install(g_pScriptGetAddonHook, 0);
	
	m_GameServerSteamAPIActivated.Add(g_pSource2Server);
	m_StartupServer.Add(g_pNetworkServerService);
	m_ClientConnect.Add(g_pSource2GameClients);
	m_ClientDisconnect.Add(g_pSource2GameClients);
	m_ClientActive.Add(g_pSource2GameClients);
	m_CanHLTVClientConnect.Add(g_pSource2GameClients);
	m_GameFrame.Add(g_pSource2Server);
	m_PostEvent.Add(g_pGameEventSystem);

	g_pEventMgrVtbl = serverModule.FindVirtualTable("CGameEventManager");

	if (!g_pEventMgrVtbl)
		return false;

	AddGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);
	g_iLoadEventsFromFileHookId = 1; // non-zero = hooked (compat with old id check)

	// Register Steam callbacks on load (upstream b1e4275), not only on late-load.
	if (g_pEngineServer->IsDedicatedServer())
	{
		m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);
		m_CallbackSteamServersConnected.Register(this, &MultiAddonManager::OnSteamServersConnected);
	}

	if (late)
	{
		g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
		gpGlobals = g_pEngineServer->GetServerGlobals();
	}

	META_CONVAR_REGISTER(FCVAR_RELEASE);

	ParseCfg();

	Message("Plugin loaded successfully!\n");

	return true;
}

bool MultiAddonManager::Unload(char *error, size_t maxlen)
{
	ClearAddons();

	m_CallbackDownloadItemResult.Unregister();
	m_CallbackSteamServersConnected.Unregister();

	m_GameServerSteamAPIActivated.Remove(g_pSource2Server);
	m_StartupServer.Remove(g_pNetworkServerService);
	m_ClientConnect.Remove(g_pSource2GameClients);
	m_ClientDisconnect.Remove(g_pSource2GameClients);
	m_ClientActive.Remove(g_pSource2GameClients);
	m_CanHLTVClientConnect.Remove(g_pSource2GameClients);
	m_GameFrame.Remove(g_pSource2Server);
	m_PostEvent.Remove(g_pGameEventSystem);
	RemoveGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);
	g_pEventMgrVtbl = nullptr;
	g_iLoadEventsFromFileHookId = -1;

	if (g_pSetPendingHostStateRequest)
	{
		funchook_uninstall(g_pSetPendingHostStateRequest, 0);
		funchook_destroy(g_pSetPendingHostStateRequest);
	}

	if (g_pSendNetMessageHook_ServerSideClient)
	{
		funchook_uninstall(g_pSendNetMessageHook_ServerSideClient, 0);
		funchook_destroy(g_pSendNetMessageHook_ServerSideClient);
	}

	if (g_pSendNetMessageHook_HLTVClient)
	{
		funchook_uninstall(g_pSendNetMessageHook_HLTVClient, 0);
		funchook_destroy(g_pSendNetMessageHook_HLTVClient);
	}

	if (g_pReplyConnectionHook)
	{
		funchook_uninstall(g_pReplyConnectionHook, 0);
		funchook_destroy(g_pReplyConnectionHook);
	}

	if (g_pScriptGetAddonHook)
	{
		funchook_uninstall(g_pScriptGetAddonHook, 0);
		funchook_destroy(g_pScriptGetAddonHook);
	}
	
	return true;
}

void *MultiAddonManager::OnMetamodQuery(const char *iface, int *ret)
{
	if (V_strcmp(iface, MULTIADDONMANAGER_INTERFACE))
	{
		if (ret)
			*ret = META_IFACE_FAILED;

		return nullptr;
	}

	if (ret)
		*ret = META_IFACE_OK;

	return static_cast<IMultiAddonManager*>(&g_MultiAddonManager);
}

void MultiAddonManager::ParseCfg()
{
	// Expected path: <Plat_GetGameDirectory()>/csgo/cfg/multiaddonmanager/multiaddonmanager.cfg
	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s/csgo/cfg/multiaddonmanager/multiaddonmanager.cfg", Plat_GetGameDirectory());
	std::ifstream cfgFile(szPath);

	if (!cfgFile.is_open())
	{
		Panic("Unable to open & execute config at \"%s\"\n", szPath);
		return;
	}

	Message("Executing config at \"%s\"\n", szPath);

	std::string strCommand;

	while (std::getline(cfgFile, strCommand))
	{
		if (!strCommand.empty() && strCommand.back() == '\r')
			strCommand.pop_back();

		// Apply immediately. ServerCommand only queues into the buffer and does not
		// reliably strip trailing // comments, so mm_extra_addons stayed empty and
		// RefreshAddons() logged an empty list.
		CCommand args;
		if (!args.Tokenize(strCommand.c_str()))
			continue;

		const char *pszCommand = args.Arg(0);
		if (!pszCommand || !*pszCommand)
			continue;

		ConVarRefAbstract cvarRef(pszCommand, true);
		if (cvarRef.IsValidRef())
		{
			cvarRef.SetString(args.ArgC() > 1 ? args.Arg(1) : "");
			continue;
		}

		ConCommandRef cmdRef(pszCommand, true);
		if (cmdRef.IsValidRef())
		{
			CCommandContext ctx(CT_FIRST_SPLITSCREEN_CLIENT, -1);
			cmdRef.Dispatch(ctx, args);
		}
	}

	Message("Config applied, mm_extra_addons = \"%s\"\n", mm_extra_addons.Get().Get());
}

static void FormatItemState(uint32 state, char *buf, size_t len)
{
	if (!buf || len == 0)
		return;

	buf[0] = '\0';
	auto append = [&](const char *flag)
	{
		if (buf[0])
			V_strcat(buf, "|", len);
		V_strcat(buf, flag, len);
	};

	if (state == k_EItemStateNone)
	{
		V_strncpy(buf, "None", len);
		return;
	}
	if (state & k_EItemStateSubscribed) append("Subscribed");
	if (state & k_EItemStateLegacyItem) append("Legacy");
	if (state & k_EItemStateInstalled) append("Installed");
	if (state & k_EItemStateNeedsUpdate) append("NeedsUpdate");
	if (state & k_EItemStateDownloading) append("Downloading");
	if (state & k_EItemStateDownloadPending) append("DownloadPending");
	if (!buf[0])
		V_snprintf(buf, len, "0x%X", state);
}

void MultiAddonManager::PrintStatus()
{
	char szCfgPath[MAX_PATH];
	V_snprintf(szCfgPath, sizeof(szCfgPath), "%s/csgo/cfg/multiaddonmanager/multiaddonmanager.cfg", Plat_GetGameDirectory());

	std::ifstream cfgFile(szCfgPath);
	const bool bCfgExists = cfgFile.good();
	cfgFile.close();

	const bool bDedicated = g_pEngineServer->IsDedicatedServer();
	ISteamGameServer *pGameServer = bDedicated ? SteamGameServer() : nullptr;
	const bool bLoggedOn = !bDedicated || (pGameServer && pGameServer->BLoggedOn());
	ISteamUGC *pUGC = GetSteamUGC();
	const bool bSteamReady = IsSteamReadyForAddons();

	Message("========== MultiAddonManager status ==========\n");
	Message("Version: %s (%s)\n", GetVersion(), GetDate());
	Message("Game directory: %s\n", Plat_GetGameDirectory());
	Message("Config path: %s\n", szCfgPath);
	Message("Config file: %s\n", bCfgExists ? "found" : "MISSING");
	Message("Server: %s\n", bDedicated ? "dedicated" : "listen");
	Message("Map: %s\n", gpGlobals ? gpGlobals->mapname.ToCStr() : "(none)");
	Message("Workshop map: %s\n", m_sCurrentWorkshopMap.empty() ? "(none)" : m_sCurrentWorkshopMap.c_str());
	Message("Loading workshop map: %s\n", m_sLoadingWorkshopMap.empty() ? "(none)" : m_sLoadingWorkshopMap.c_str());

	Message("--- Steam ---\n");
	Message("UGC interface: %s\n", pUGC ? "ready" : "not ready");
	Message("SteamServersConnected callback: %s\n", m_bSteamConnected ? "yes" : "no");
	Message("Gameserver logged on: %s\n", bLoggedOn ? "yes" : "no");
	Message("Steam ready for addons: %s\n", bSteamReady ? "yes" : "no");
	Message("Pre-login download retries: %d\n", m_PreSteamLoginDownloads.Count());
	Message("Pending refresh: %s (reload map: %s)\n",
		m_bPendingSteamRefresh ? "yes" : "no",
		m_bPendingReloadMap ? "yes" : "no");
	if (m_flSteamApiActivatedTime > 0.0)
	{
		const double flElapsed = Plat_FloatTime() - m_flSteamApiActivatedTime;
		Message("Steam activated %.1fs ago (refresh delay %.1fs)\n", flElapsed, mm_steam_refresh_delay.Get());
	}
	else
	{
		Message("Steam API not activated yet\n");
	}

	Message("--- ConVars ---\n");
	Message("mm_extra_addons = \"%s\"\n", mm_extra_addons.Get().Get());
	Message("mm_client_extra_addons = \"%s\"\n", mm_client_extra_addons.Get().Get());
	Message("mm_addon_mount_download = %d\n", mm_addon_mount_download.Get() ? 1 : 0);
	Message("mm_steam_refresh_delay = %.1f\n", mm_steam_refresh_delay.Get());
	Message("mm_addon_debug = %d\n", mm_addon_debug.Get() ? 1 : 0);

	Message("--- Lists ---\n");
	Message("Configured extras: %d | Mounted: %d | Client-only: %d\n",
		m_ExtraAddons.Count(), m_MountedAddons.Count(), m_GlobalClientAddons.Count());
	Message("Download queue: %d | Important downloads: %d\n",
		m_DownloadQueue.Count(), m_ImportantDownloads.Count());

	if (m_ExtraAddons.Count() == 0)
	{
		Message("No extra addons configured.\n");
	}
	else
	{
		Message("--- Extra addons ---\n");
		FOR_EACH_VEC(m_ExtraAddons, i)
		{
			const char *pszAddon = m_ExtraAddons[i].c_str();
			const bool bMounted = m_MountedAddons.Find(pszAddon) != -1;
			const bool bWorkshopMap = (m_sCurrentWorkshopMap == pszAddon);

			char szPath[MAX_PATH] = {};
			const bool bPathOk = ResolveAddonPath(pszAddon, szPath, sizeof(szPath));

			uint64 nSize = 0;
			uint32 nTime = 0;
			char szInstallFolder[MAX_PATH] = {};
			PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
			const bool bMeta = iAddon && GetAddonInstallMeta(iAddon, nSize, nTime, szInstallFolder, sizeof(szInstallFolder));

			char szState[128] = {};
			if (pUGC && iAddon)
				FormatItemState(pUGC->GetItemState(iAddon), szState, sizeof(szState));
			else
				V_strncpy(szState, "n/a", sizeof(szState));

			Message("[%d] %s | mounted=%s%s | files=%s | ugc=%s\n",
				i,
				pszAddon,
				bMounted ? "yes" : "no",
				bWorkshopMap ? " (workshop map)" : "",
				bPathOk ? "ok" : "MISSING",
				szState);

			if (bPathOk)
				Message("    search path: %s\n", szPath);
			if (bMeta)
			{
				Message("    install: %s\n", szInstallFolder[0] ? szInstallFolder : "(unknown)");
				Message("    size=%llu bytes, timestamp=%u\n",
					static_cast<unsigned long long>(nSize), nTime);
			}

			auto metaIt = m_AddonMountMeta.find(pszAddon);
			if (metaIt != m_AddonMountMeta.end())
			{
				Message("    mount meta: path=%s size=%llu ts=%u\n",
					metaIt->second.path.c_str(),
					static_cast<unsigned long long>(metaIt->second.sizeOnDisk),
					metaIt->second.timeStamp);
			}

			auto nextIt = m_AddonNextMountAttempt.find(pszAddon);
			if (nextIt != m_AddonNextMountAttempt.end() && Plat_FloatTime() < nextIt->second)
			{
				Message("    next mount retry in %.1fs\n", nextIt->second - Plat_FloatTime());
			}
		}
	}

	if (m_MountedAddons.Count() > 0)
	{
		Message("--- Mounted (plugin) ---\n");
		FOR_EACH_VEC(m_MountedAddons, i)
			Message("  %s\n", m_MountedAddons[i].c_str());
	}

	if (m_GlobalClientAddons.Count() > 0)
	{
		Message("--- Client-only addons ---\n");
		FOR_EACH_VEC(m_GlobalClientAddons, i)
			Message("  %s\n", m_GlobalClientAddons[i].c_str());
	}

	Message("==============================================\n");
}

void MultiAddonManager::BuildAddonPath(const char *pszAddon, char *buf, size_t len, bool bLegacy = false)
{
	// The workshop on a dedicated server is stored relative to the working directory for whatever reason
	static CBufferStringGrowable<MAX_PATH> s_sWorkingDir;
	ExecuteOnce(g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, s_sWorkingDir, 1));

	V_snprintf(buf, len, "%ssteamapps/workshop/content/730/%s/%s%s.vpk", s_sWorkingDir.Get(), pszAddon, pszAddon, bLegacy ? "" : "_dir");
}

static void NormalizeDirPath(char *pszPath)
{
	if (!pszPath || !*pszPath)
		return;

	size_t nLen = V_strlen(pszPath);
	while (nLen > 1 && (pszPath[nLen - 1] == '/' || pszPath[nLen - 1] == '\\'))
	{
		pszPath[nLen - 1] = '\0';
		--nLen;
	}
}

static bool FileExistsAny(const char *pszPath)
{
	if (!pszPath || !*pszPath)
		return false;

	if (g_pFullFileSystem->FileExists(pszPath))
		return true;

	return g_pFullFileSystem->FileExists(pszPath, "GAME");
}

// AddSearchPath uses the non-_dir stem; multi-chunk workshop addons only have *_dir.vpk on disk.
static bool AddonSearchPathExists(const char *pszPath)
{
	if (FileExistsAny(pszPath))
		return true;

	if (!pszPath || !*pszPath)
		return false;

	const char *pszExt = V_stristr(pszPath, ".vpk");
	if (!pszExt)
		return false;

	char szDirVpk[MAX_PATH];
	const size_t nStemLen = static_cast<size_t>(pszExt - pszPath);
	if (nStemLen + 8 >= sizeof(szDirVpk)) // "_dir.vpk"
		return false;

	V_strncpy(szDirVpk, pszPath, nStemLen + 1);
	V_strcat(szDirVpk, "_dir.vpk", sizeof(szDirVpk));
	return FileExistsAny(szDirVpk);
}

static bool TryAddonVpkInDir(const char *pszDir, const char *pszAddon, char *buf, size_t len)
{
	if (!pszDir || !*pszDir || !pszAddon || !*pszAddon)
		return false;

	char szDir[MAX_PATH];
	V_strncpy(szDir, pszDir, sizeof(szDir));
	NormalizeDirPath(szDir);

	char szDirVpk[MAX_PATH];
	char szPlainVpk[MAX_PATH];
	V_snprintf(szDirVpk, sizeof(szDirVpk), "%s/%s_dir.vpk", szDir, pszAddon);
	V_snprintf(szPlainVpk, sizeof(szPlainVpk), "%s/%s.vpk", szDir, pszAddon);

	if (FileExistsAny(szDirVpk))
	{
		// Filesystem appends suffixes; AddSearchPath expects the non-_dir stem
		V_snprintf(buf, len, "%s/%s.vpk", szDir, pszAddon);
		return true;
	}

	if (FileExistsAny(szPlainVpk))
	{
		V_strncpy(buf, szPlainVpk, len);
		return true;
	}

	return false;
}

bool MultiAddonManager::ResolveAddonPath(const char *pszAddon, char *buf, size_t len)
{
	if (!pszAddon || !*pszAddon || !buf || len == 0)
		return false;

	buf[0] = '\0';

	PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
	char szInstallFolder[MAX_PATH] = {};
	uint64 nSizeOnDisk = 0;
	uint32 nTimeStamp = 0;
	if (iAddon && GetAddonInstallMeta(iAddon, nSizeOnDisk, nTimeStamp, szInstallFolder, sizeof(szInstallFolder)))
	{
		if (TryAddonVpkInDir(szInstallFolder, pszAddon, buf, len))
			return true;
	}

	static CBufferStringGrowable<MAX_PATH> s_sExecutablePath;
	ExecuteOnce(g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, s_sExecutablePath, 1));

	char szBase[MAX_PATH];
	V_strncpy(szBase, s_sExecutablePath.Get(), sizeof(szBase));
	NormalizeDirPath(szBase);

	// Walk parents of EXECUTABLE_PATH: bin/linuxsteamrt64 -> game -> repo root, etc.
	for (int iDepth = 0; iDepth < 6 && szBase[0]; ++iDepth)
	{
		char szWorkshopDir[MAX_PATH];
		V_snprintf(szWorkshopDir, sizeof(szWorkshopDir), "%s/steamapps/workshop/content/730/%s", szBase, pszAddon);
		if (TryAddonVpkInDir(szWorkshopDir, pszAddon, buf, len))
			return true;

		char *pszSlash = const_cast<char *>(V_strrchr(szBase, '/'));
		if (!pszSlash)
			pszSlash = const_cast<char *>(V_strrchr(szBase, '\\'));
		if (!pszSlash)
			break;
		*pszSlash = '\0';
	}

	BuildAddonPath(pszAddon, buf, len, false);
	if (FileExistsAny(buf))
	{
		BuildAddonPath(pszAddon, buf, len, true);
		return true;
	}

	BuildAddonPath(pszAddon, buf, len, true);
	if (FileExistsAny(buf))
		return true;

	return false;
}

bool MultiAddonManager::GetAddonInstallMeta(PublishedFileId_t iAddon, uint64 &nSizeOnDisk, uint32 &nTimeStamp, char *pszFolder, size_t nFolderLen)
{
	nSizeOnDisk = 0;
	nTimeStamp = 0;
	if (pszFolder && nFolderLen)
		pszFolder[0] = '\0';

	ISteamUGC *pUGC = GetSteamUGC();
	if (!pUGC)
		return false;

	char szFolder[MAX_PATH] = {};
	if (!pUGC->GetItemInstallInfo(iAddon, &nSizeOnDisk, szFolder, sizeof(szFolder), &nTimeStamp))
		return false;

	if (pszFolder && nFolderLen)
		V_strncpy(pszFolder, szFolder, nFolderLen);

	return true;
}

bool MultiAddonManager::IsSteamReadyForAddons()
{
	if (!GetSteamUGC() || m_bPendingSteamRefresh)
		return false;

	if (!g_pEngineServer->IsDedicatedServer())
		return true;

	ISteamGameServer *pGameServer = SteamGameServer();
	return pGameServer && pGameServer->BLoggedOn();
}

bool MultiAddonManager::MountAddon(const char *pszAddon, bool bAddToTail, bool bCheckForUpdates)
{
	if (!pszAddon || !*pszAddon)
		return false;

	CUtlVector<std::string> serverMountedAddons;
	StringToVector(this->m_sCurrentWorkshopMap.c_str(), serverMountedAddons);
	if (serverMountedAddons.Find(pszAddon) != -1)
	{
		Message("%s: Addon %s is already mounted by the server\n", __func__, pszAddon);
		return false;
	}

	ISteamUGC *pUGC = GetSteamUGC();
	if (!pUGC)
	{
		Panic("%s: Steam UGC is not ready, cannot mount addon %s\n", __func__, pszAddon);
		return false;
	}

	PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
	uint32 iAddonState = pUGC->GetItemState(iAddon);

	if (iAddonState & k_EItemStateLegacyItem)
	{
		Message("%s: Addon %s is not compatible with Source 2, skipping\n", __func__, pszAddon);
		return false;
	}

	if (bCheckForUpdates && (iAddonState & k_EItemStateNeedsUpdate))
	{
		Message("%s: Addon %s has an update available, queuing a download\n", __func__, pszAddon);
		DownloadAddon(pszAddon, true, true);
		return false;
	}

	if (!(iAddonState & k_EItemStateInstalled))
	{
		Message("%s: Addon %s is not installed, queuing a download\n", __func__, pszAddon);
		DownloadAddon(pszAddon, true, true);
		return false;
	}
	else if (bCheckForUpdates && mm_addon_mount_download.Get())
	{
		// Queue a download anyway in case the addon got an update and the server desires this, but don't reload the map when done
		DownloadAddon(pszAddon, false, true);
	}

	char pszPath[MAX_PATH];
	if (!ResolveAddonPath(pszAddon, pszPath, sizeof(pszPath)))
	{
		char szInstallFolder[MAX_PATH] = {};
		uint64 nIgnoredSize = 0;
		uint32 nIgnoredTime = 0;
		GetAddonInstallMeta(iAddon, nIgnoredSize, nIgnoredTime, szInstallFolder, sizeof(szInstallFolder));

		Panic("%s: Addon %s not found on disk (install_folder='%s', tried='%s'), queuing re-download\n",
			__func__, pszAddon, szInstallFolder[0] ? szInstallFolder : "<none>", pszPath[0] ? pszPath : "<empty>");
		DownloadAddon(pszAddon, true, true);
		m_AddonNextMountAttempt[pszAddon] = Plat_FloatTime() + 15.0;
		return false;
	}

	uint64 nSizeOnDisk = 0;
	uint32 nTimeStamp = 0;
	GetAddonInstallMeta(iAddon, nSizeOnDisk, nTimeStamp);

	if (m_MountedAddons.Find(pszAddon) != -1)
	{
		auto it = m_AddonMountMeta.find(pszAddon);
		const bool bHasMeta = it != m_AddonMountMeta.end();
		const bool bPathExists = AddonSearchPathExists(pszPath);
		const bool bMetaChanged = bHasMeta && (it->second.sizeOnDisk != nSizeOnDisk || it->second.timeStamp != nTimeStamp);

		if (bPathExists && bHasMeta && !bMetaChanged)
			return true;

		Message("%s: Addon %s is mounted but %s, remounting\n", __func__, pszAddon,
			!bPathExists ? "files are missing" : "size/timestamp changed");
		UnmountAddon(pszAddon);
	}

	Message("Mounting addon %s -> %s (size=%llu, timestamp=%u)\n",
		pszAddon, pszPath, (unsigned long long)nSizeOnDisk, nTimeStamp);

	g_pFullFileSystem->AddSearchPath(pszPath, "GAME", bAddToTail ? PATH_ADD_TO_TAIL : PATH_ADD_TO_HEAD, SEARCH_PATH_PRIORITY_VPK);
	m_MountedAddons.AddToTail(pszAddon);
	m_AddonMountMeta[pszAddon] = { pszPath, nSizeOnDisk, nTimeStamp };
	m_AddonNextMountAttempt.erase(pszAddon);

	Message("Addon %s mounted successfully\n", pszAddon);
	return true;
}

bool MultiAddonManager::UnmountAddon(const char *pszAddon)
{
	if (!pszAddon || !*pszAddon)
		return false;

	char path[MAX_PATH];
	bool bRemoved = false;

	// Prefer the path we actually mounted (non-_dir / legacy style)
	auto it = m_AddonMountMeta.find(pszAddon);
	if (it != m_AddonMountMeta.end() && !it->second.path.empty())
	{
		V_strncpy(path, it->second.path.c_str(), sizeof(path));
		bRemoved = g_pFullFileSystem->RemoveSearchPath(path, "GAME");
	}

	if (!bRemoved)
	{
		BuildAddonPath(pszAddon, path, sizeof(path), true);
		bRemoved = g_pFullFileSystem->RemoveSearchPath(path, "GAME");
	}

	if (!bRemoved)
	{
		BuildAddonPath(pszAddon, path, sizeof(path), false);
		bRemoved = g_pFullFileSystem->RemoveSearchPath(path, "GAME");
	}

	m_MountedAddons.FindAndFastRemove(pszAddon);
	m_AddonMountMeta.erase(pszAddon);

	if (bRemoved)
		Message("Removing search path: %s\n", path);
	else
		Message("Addon %s untracked (search path already gone)\n", pszAddon);

	return true;
}

void MultiAddonManager::PrintDownloadProgress()
{
	if (m_DownloadQueue.Count() == 0)
		return;

	uint64 iBytesDownloaded = 0;
	uint64 iTotalBytes = 0;

	if (!GetSteamUGC()->GetItemDownloadInfo(m_DownloadQueue.Head(), &iBytesDownloaded, &iTotalBytes) || !iTotalBytes)
		return;

	double flMBDownloaded = (double)iBytesDownloaded / 1024 / 1024;
	double flTotalMB = (double)iTotalBytes / 1024 / 1024;

	double flProgress = (double)iBytesDownloaded / (double)iTotalBytes;
	flProgress *= 100.f;

	Message("Downloading addon %lli: %.2f/%.2f MB (%.2f%%)\n", m_DownloadQueue.Head(), flMBDownloaded, flTotalMB, flProgress);
}

// bImportant adds downloads to the pending list, which will reload the current map once the list is exhausted
// bForce will initiate a download even if the addon already exists and is updated
// Internally, downloads are queued up and processed one at a time
bool MultiAddonManager::DownloadAddon(const char *pszAddon, bool bImportant, bool bForce)
{
	if (!GetSteamUGC())
	{
		Panic("%s: Cannot download addons as the Steam API is not initialized\n", __func__);
		return false;
	}

	PublishedFileId_t addon = V_StringToUint64(pszAddon, 0);

	if (addon == 0)
	{
		Panic("%s: Invalid addon %s\n", __func__, pszAddon);
		return false;
	}

	if (m_DownloadQueue.Check(addon))
	{
		if (mm_addon_debug.Get())
			Message("%s: Addon %s is already queued for download\n", __func__, pszAddon);
		return false;
	}

	uint32 nItemState = GetSteamUGC()->GetItemState(addon);

	if (!bForce && (nItemState & k_EItemStateInstalled) && !(nItemState & k_EItemStateNeedsUpdate))
	{
		Message("Addon %lli is already installed\n", addon);
		return true;
	}

	if (!GetSteamUGC()->DownloadItem(addon, false))
	{
		Panic("%s: Addon download for %lli failed to start, addon ID is invalid or server is not logged on Steam\n", __func__, addon);

		// Most likely the server hasn't logged on to Steam yet; retry in OnSteamServersConnected
		AddonDownloadInfo_t pending = { pszAddon, bImportant, bForce };
		if (m_PreSteamLoginDownloads.Find(pending) == -1)
			m_PreSteamLoginDownloads.AddToTail(pending);

		return false;
	}
	
	if (bImportant && m_ImportantDownloads.Find(addon) == -1)
		m_ImportantDownloads.AddToTail(addon);

	m_DownloadQueue.Insert(addon);

	Message("Addon download started for %lli\n", addon);

	return true;
}

void MultiAddonManager::OnSteamServersConnected(SteamServersConnected_t *pResult)
{
	Message("Logged on to Steam\n");

	m_bSteamConnected = true;

	if (m_PreSteamLoginDownloads.Count() == 0)
	{
		// Still request a deferred refresh — downloads may already be queued elsewhere
		RequestRefreshAddons(false);
		return;
	}

	Message("Retrying %i pending addon download(s)\n", m_PreSteamLoginDownloads.Count());

	CUtlVector<AddonDownloadInfo_t> pending;
	pending.AddVectorToTail(m_PreSteamLoginDownloads);
	m_PreSteamLoginDownloads.RemoveAll();

	FOR_EACH_VEC(pending, i)
		DownloadAddon(pending[i].addon.c_str(), pending[i].bImportant, pending[i].bForce);

	RequestRefreshAddons(false);
}

void MultiAddonManager::RefreshAddons(bool bReloadMap)
{
	if (!GetSteamUGC())
		return;

	Message("Refreshing addons (%s)\n", VectorToString(m_ExtraAddons).c_str());

	// Remove our paths first in case addons were switched
	while (m_MountedAddons.Count() > 0)
		UnmountAddon(m_MountedAddons.Tail().c_str());

	bool bAllAddonsMounted = true;

	FOR_EACH_VEC(m_ExtraAddons, i)
	{
		if (!MountAddon(m_ExtraAddons[i].c_str()))
			bAllAddonsMounted = false;
	}

	if (bAllAddonsMounted && bReloadMap)
		ReloadMap();
}

void MultiAddonManager::RequestRefreshAddons(bool bReloadMap)
{
	const bool bDedicated = g_pEngineServer->IsDedicatedServer();
	ISteamGameServer *pGameServer = bDedicated ? SteamGameServer() : nullptr;
	const bool bLoggedOn = !bDedicated || (pGameServer && pGameServer->BLoggedOn());
	const bool bDelayElapsed = m_flSteamApiActivatedTime <= 0.0
		|| Plat_FloatTime() - m_flSteamApiActivatedTime >= mm_steam_refresh_delay.Get();

	if (!bLoggedOn || !bDelayElapsed || !GetSteamUGC())
	{
		m_bPendingSteamRefresh = true;
		m_bPendingReloadMap = m_bPendingReloadMap || bReloadMap;
		if (m_flSteamApiActivatedTime <= 0.0)
			m_flSteamApiActivatedTime = Plat_FloatTime();
		Message("Deferring addon refresh until Steam logon is ready\n");
		return;
	}

	RefreshAddons(bReloadMap);
}

void MultiAddonManager::CheckMountedAddons()
{
	if (!IsSteamReadyForAddons() || m_ExtraAddons.Count() == 0)
		return;

	const double flNow = Plat_FloatTime();

	FOR_EACH_VEC(m_ExtraAddons, i)
	{
		const char *pszAddon = m_ExtraAddons[i].c_str();
		PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
		if (!iAddon)
			continue;

		auto nextIt = m_AddonNextMountAttempt.find(pszAddon);
		if (nextIt != m_AddonNextMountAttempt.end() && flNow < nextIt->second)
			continue;

		uint32 iAddonState = GetSteamUGC()->GetItemState(iAddon);

		if (iAddonState & k_EItemStateLegacyItem)
			continue;

		if (iAddonState & (k_EItemStateDownloading | k_EItemStateDownloadPending))
			continue;

		if (m_DownloadQueue.Check(iAddon))
			continue;

		if (iAddonState & k_EItemStateNeedsUpdate)
		{
			Message("Addon %s needs an update, queuing a download\n", pszAddon);
			if (m_MountedAddons.Find(pszAddon) != -1)
				UnmountAddon(pszAddon);
			DownloadAddon(pszAddon, true, true);
			m_AddonNextMountAttempt[pszAddon] = flNow + 15.0;
			continue;
		}

		if (!(iAddonState & k_EItemStateInstalled))
		{
			if (m_MountedAddons.Find(pszAddon) != -1)
			{
				Message("Addon %s was mounted but is no longer installed, remounting\n", pszAddon);
				UnmountAddon(pszAddon);
			}
			if (!MountAddon(pszAddon))
				m_AddonNextMountAttempt[pszAddon] = flNow + 15.0;
			continue;
		}

		char pszPath[MAX_PATH];
		const bool bPathResolved = ResolveAddonPath(pszAddon, pszPath, sizeof(pszPath));
		const bool bMounted = m_MountedAddons.Find(pszAddon) != -1;

		if (!bMounted)
		{
			Message("Addon %s is listed but not mounted, mounting\n", pszAddon);
			if (!MountAddon(pszAddon))
				m_AddonNextMountAttempt[pszAddon] = flNow + 15.0;
			continue;
		}

		// ResolveAddonPath already verified *_dir.vpk / *.vpk exists. Do not FileExists the
		// AddSearchPath stem (*.vpk) — multi-chunk addons only have *_dir.vpk on disk.
		if (!bPathResolved)
		{
			Message("Addon %s was mounted but files are missing, remounting\n", pszAddon);
			UnmountAddon(pszAddon);
			if (!MountAddon(pszAddon))
				m_AddonNextMountAttempt[pszAddon] = flNow + 15.0;
			continue;
		}

		uint64 nSizeOnDisk = 0;
		uint32 nTimeStamp = 0;
		if (!GetAddonInstallMeta(iAddon, nSizeOnDisk, nTimeStamp))
			continue;

		auto it = m_AddonMountMeta.find(pszAddon);
		if (it == m_AddonMountMeta.end())
		{
			m_AddonMountMeta[pszAddon] = { pszPath, nSizeOnDisk, nTimeStamp };
			continue;
		}

		if (it->second.sizeOnDisk != nSizeOnDisk || it->second.timeStamp != nTimeStamp)
		{
			Message("Addon %s size/timestamp changed (size %llu->%llu, time %u->%u), remounting\n",
				pszAddon,
				(unsigned long long)it->second.sizeOnDisk, (unsigned long long)nSizeOnDisk,
				it->second.timeStamp, nTimeStamp);
			UnmountAddon(pszAddon);
			if (!MountAddon(pszAddon))
				m_AddonNextMountAttempt[pszAddon] = flNow + 15.0;
		}
	}
}

void MultiAddonManager::ClearAddons()
{
	m_ExtraAddons.RemoveAll();

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = "";
	
	while (m_MountedAddons.Count() > 0)
		UnmountAddon(m_MountedAddons.Tail().c_str());
}

KHook::Return<void> MultiAddonManager::Hook_GameServerSteamAPIActivated(ISource2Server*)
{
	// This is only intended for dedicated servers
	if (!g_pEngineServer->IsDedicatedServer())
		return { KHook::Action::Ignore };

	Message("Steam API Activated\n");

	// GameServerSteamAPIActivated fires before the gameserver finishes logging on to Steam.
	// Download callbacks are already registered in Load; wait for logon / mm_steam_refresh_delay.
	m_flSteamApiActivatedTime = Plat_FloatTime();
	m_bPendingReloadMap = true;
	RequestRefreshAddons(true);

	return { KHook::Action::Ignore };
}

void MultiAddonManager::ReloadMap()
{
	char cmd[MAX_PATH];

	// Using the concommand here as g_pEngineServer->ChangeLevel doesn't unmount workshop maps and we wanna be clean.
	// See Hook_SetPendingHostStateRequest's comment for more details.
	// Community maps are treated like workshop maps but they should still be loaded using changelevel
	if (m_sCurrentWorkshopMap.empty() || g_pFullFileSystem->IsDirectory(m_sCurrentWorkshopMap.c_str(), "OFFICIAL_ADDONS"))
		V_snprintf(cmd, sizeof(cmd), "changelevel %s", gpGlobals->mapname.ToCStr());
	else
		V_snprintf(cmd, sizeof(cmd), "host_workshop_map %s", m_sCurrentWorkshopMap.c_str());

	g_pEngineServer->ServerCommand(cmd);
}

void MultiAddonManager::OnAddonDownloaded(DownloadItemResult_t *pResult)
{
	if (pResult->m_eResult == k_EResultOK)
	{
		Message("Addon %lli downloaded successfully\n", pResult->m_nPublishedFileId);
	}
	else
	{
		static const int iNumMessages = static_cast<int>(sizeof(g_SteamErrorMessages) / sizeof(g_SteamErrorMessages[0]));
		const char *pszReason = (pResult->m_eResult >= 0 && pResult->m_eResult < iNumMessages)
			? g_SteamErrorMessages[pResult->m_eResult]
			: "Unknown error";
		Panic("Addon %lli download failed with reason \"%s\" (%i)\n", pResult->m_nPublishedFileId, pszReason, pResult->m_eResult);
	}

	// Drop any matching pre-login retry entry
	FOR_EACH_VEC_BACK(m_PreSteamLoginDownloads, i)
	{
		if (V_StringToUint64(m_PreSteamLoginDownloads[i].addon.c_str(), 0) == pResult->m_nPublishedFileId)
			m_PreSteamLoginDownloads.Remove(i);
	}

	// This download isn't triggered by us, don't do anything
	if (!m_DownloadQueue.Check(pResult->m_nPublishedFileId))
		return;

	m_DownloadQueue.RemoveAtHead();

	bool bFound = m_ImportantDownloads.FindAndRemove(pResult->m_nPublishedFileId);

	// Remount updated/downloaded addons that belong to our extra list
	if (pResult->m_eResult == k_EResultOK)
	{
		FOR_EACH_VEC(m_ExtraAddons, i)
		{
			if (V_StringToUint64(m_ExtraAddons[i].c_str(), 0) != pResult->m_nPublishedFileId)
				continue;

			const char *pszAddon = m_ExtraAddons[i].c_str();
			if (m_MountedAddons.Find(pszAddon) != -1)
				UnmountAddon(pszAddon);

			if (!MountAddon(pszAddon, false, false))
				Message("Failed to mount addon %s after download\n", pszAddon);
			break;
		}
	}

	// That was the last important download, now reload the map
	if (bFound && m_ImportantDownloads.Count() == 0)
	{
		Message("All addon downloads finished, reloading map %s\n", gpGlobals->mapname);
		ReloadMap();
	}
}

bool MultiAddonManager::AddAddon(const char *pszAddon, bool bRefresh)
{
	if (m_ExtraAddons.Find(pszAddon) != -1)
	{
		Panic("Addon %s is already in the list!\n", pszAddon);
		return false;
	}

	Message("Adding %s to addon list\n", pszAddon);

	m_ExtraAddons.AddToTail(pszAddon);

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	Message("Clearing client cache due to addons changing");

	if (bRefresh)
		RefreshAddons();

	return true;
}

bool MultiAddonManager::RemoveAddon(const char *pszAddon, bool bRefresh)
{
	int index = m_ExtraAddons.Find(pszAddon);

	if (index == -1)
	{
		Panic("Addon %s is not in the list!\n", pszAddon);
		return false;
	}

	Message("Removing %s from addon list\n", pszAddon);

	m_ExtraAddons.Remove(index);

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	if (bRefresh)
		RefreshAddons();

	return true;
}

CNetMessagePB<CNETMsg_SignonState> *GetAddonSignonStateMessage(const char *pszAddon)
{
	if (!gpGlobals)
		return nullptr;

	INetworkMessageInternal *pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("SignonState");
	CNetMessagePB<CNETMsg_SignonState> *pMsg = pNetMsg->AllocateMessage()->ToPB<CNETMsg_SignonState>();
	pMsg->set_spawn_count(gpGlobals->serverCount);
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);
	pMsg->set_addons(pszAddon ? pszAddon : "");
	pMsg->set_num_server_players(GetClientList()->Count());
	for (int i = 0; i < GetClientList()->Count(); i++)
	{
		auto client = GetClientList()->Element(i);

		char const *szNetworkId = g_pEngineServer->GetPlayerNetworkIDString(client->GetPlayerSlot());

		pMsg->add_players_networkids(szNetworkId);
	}

	return pMsg;
}

bool MultiAddonManager::HasUGCConnection()
{
	return GetSteamUGC() != nullptr && m_bSteamConnected;
}

void MultiAddonManager::AddClientAddon(const char *pszAddon, uint64 steamID64, bool bRefresh)
{
	if (!steamID64)
	{
		if (m_GlobalClientAddons.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}
	
		m_GlobalClientAddons.AddToTail(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		if (g_ClientAddons[steamID64].addonsToLoad.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}

		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.AddToTail(pszAddon);
	}
	
	if (bRefresh)
	{
		CUtlVector<CServerSideClient*> &clients = *GetClientList();
		auto pMsg = GetAddonSignonStateMessage(pszAddon);
		if (!pMsg)
		{
			Panic("Failed to create signon state message for %s\n", pszAddon);
			return;
		}
		FOR_EACH_VEC(clients, i)
		{
			CServerSideClient *pClient = clients[i];
			uint64 clientSteamId = pClient->GetClientSteamID().ConvertToUint64();

			if (steamID64 != 0 && clientSteamId != steamID64)
				continue;

			// Client is already loading, telling them to reload now will actually just disconnect them.
			if (pClient->GetSignonState() == SIGNONSTATE_CHANGELEVEL)
			{
				if (steamID64)
					break;
				continue;
			}
			// Client still has addons to load anyway, they don't need to be told to reload
			if (!g_ClientAddons[clientSteamId].currentPendingAddon.empty())
			{
				if (steamID64)
					break;
				continue;
			}

			ClientAddonInfo_t &clientInfo = g_ClientAddons[clientSteamId];

			CUtlVector<std::string> addons;
			GetClientAddons(addons, clientSteamId);

			FOR_EACH_VEC(clientInfo.downloadedAddons, j)
				addons.FindAndRemove(clientInfo.downloadedAddons[j]);

			if (!addons.Count())
			{
				if (steamID64)
					break;
				continue;
			}

			clientInfo.currentPendingAddon = addons.Head();
			pClient->GetNetChannel()->SendNetMessage(pMsg, BUF_RELIABLE);

			if (steamID64)
				break;
		}
		delete pMsg;
	}
}

void MultiAddonManager::RemoveClientAddon(const char *pszAddon, uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.FindAndRemove(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.FindAndRemove(pszAddon);
	}
}

void MultiAddonManager::ClearClientAddons(uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.RemoveAll();
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.RemoveAll();
	}
}

void MultiAddonManager::GetClientAddons(CUtlVector<std::string> &addons, uint64 steamID64)
{
	addons.RemoveAll();
	
	if (!GetCurrentWorkshopMap().empty())
		addons.AddToTail(GetCurrentWorkshopMap().c_str());
	// The list of mounted addons should never contain the workshop map.
	addons.AddVectorToTail(m_MountedAddons);
	// Also make sure we don't have duplicates.
	FOR_EACH_VEC(m_GlobalClientAddons, i)
	{
		if (addons.Find(m_GlobalClientAddons[i].c_str()) == -1)
			addons.AddToTail(m_GlobalClientAddons[i].c_str());
	}
	// If we specify a client steamID64, check for the addons exclusive to this client as well.
	if (steamID64)
	{
		FOR_EACH_VEC(g_ClientAddons[steamID64].addonsToLoad, i)
		{
			if (addons.Find(g_ClientAddons[steamID64].addonsToLoad[i].c_str()) == -1)
				addons.AddToTail(g_ClientAddons[steamID64].addonsToLoad[i].c_str());
		}
	}
}

CON_COMMAND_F(mm_add_client_addon, "Add a workshop ID to the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.AddClientAddon(args[1]);
}

CON_COMMAND_F(mm_remove_client_addon, "Remove a workshop ID from the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.RemoveClientAddon(args[1]);
}

CON_COMMAND_F(mm_add_addon, "Add a workshop ID to the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.AddAddon(args[1]);
}

CON_COMMAND_F(mm_remove_addon, "Remove a workshop ID from the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.RemoveAddon(args[1]);
}

CON_COMMAND_F(mm_download_addon, "Download an addon manually", FCVAR_SPONLY)
{
	if (args.ArgC() != 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.DownloadAddon(args[1], false, true);
}

CON_COMMAND_F(mam_status, "Print MultiAddonManager status, config path and addon details", FCVAR_SPONLY)
{
	g_MultiAddonManager.PrintStatus();
}

CON_COMMAND_F(mm_print_searchpaths, "Print search paths", FCVAR_SPONLY)
{
	g_pFullFileSystem->PrintSearchPaths();
}

CON_COMMAND_F(mm_print_searchpaths_client, "Print search paths client-side, only usable if you're running the plugin on a listenserver", FCVAR_CLIENTDLL)
{
	g_pFullFileSystem->PrintSearchPaths();
}

KHook::Return<void> MultiAddonManager::Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t &config, ISource2WorldSession *session, const char *mapname)
{
	gpGlobals = g_pEngineServer->GetServerGlobals();
	g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();

	m_TimedOutClients.clear();

	// Upstream commits LoadingWorkshopMap in SwitchToLoop("levelload"); StartupServer is our equivalent.
	CommitLoadingWorkshopMap();

	// Remove empty paths added when there are 2+ addons, they screw up file writes
	g_pFullFileSystem->RemoveSearchPath("", "GAME");
	g_pFullFileSystem->RemoveSearchPath("", "DEFAULT_WRITE_PATH");

	// This has to be done here to replicate the behavior on clients, where they mount addons in the string order
	// So if the current map is ID 1 and extra addons are IDs 2 and 3, they would be mounted in that order with ID 3 at the top
	// Note that the actual map VPK(s) and any sub-maps like team_select will be even higher, but those usually don't contain any assets that concern us
	RequestRefreshAddons();
	return { KHook::Action::Ignore };
}

bool FASTCALL Hook_SendNetMessage(CServerSideClientBase *pClient, CNetMessage *pData, NetChannelBufType_t bufType, SendNetMessage_t pOriginalFunc)
{
	NetMessageInfo_t *info = pData->GetNetMessage()->GetNetMessageInfo();
	
	uint64 steamID64 = pClient->GetClientSteamID().ConvertToUint64();
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	
	// If we are sending a message to the client, that means the client is still active.
	clientInfo.lastActiveTime = Plat_FloatTime();

	if (info->m_MessageId != net_SignonState || !g_pEngineServer->IsDedicatedServer())
		return pOriginalFunc(pClient, pData, bufType);

	auto pMsg = pData->ToPB<CNETMsg_SignonState>();

	CUtlVector<std::string> addons;
	g_MultiAddonManager.GetClientAddons(addons, steamID64);
	
	
	if (pMsg->signon_state() == SIGNONSTATE_CHANGELEVEL)
	{
		// When switching to another map, the signon message might contain more than 1 addon.
		// This puts the client in limbo because client doesn't know how to handle multiple addons at the same time.
		CUtlVector<std::string> addonsList;
		StringToVector(pMsg->addons().c_str(), addonsList);
		if (addonsList.Count() > 1)
		{
			// If there's more than one addon, ensure that it takes the first addon (which should be the workshop map or the first custom addon)
			pMsg->set_addons(addonsList.Head());
			// Since the client will download the addon contained inside this messsage, we might as well add it to the list of client's downloaded addons.
			clientInfo.currentPendingAddon = addonsList.Head();
		}
		else if (addonsList.Count() == 1)
		{
			// Nothing to do here, the rest of the required addons can be sent later.
			clientInfo.currentPendingAddon = pMsg->addons();
		}
		
		return pOriginalFunc(pClient, pData, bufType);
	}
	FOR_EACH_VEC(clientInfo.downloadedAddons, i)
	{
		addons.FindAndRemove(clientInfo.downloadedAddons[i]);
	}
	
	// Check if client has downloaded everything.
	if (addons.Count() == 0)
	{
		return pOriginalFunc(pClient, pData, bufType);
	}

	if (mm_addon_debug.Get())
		Message("%s: Number of addons remaining to download for %lli: %d\n", __func__, steamID64, addons.Count());

	// Otherwise, send the next addon to the client.
	clientInfo.currentPendingAddon = addons.Head();
	pMsg->set_addons(addons.Head().c_str());
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);

	return pOriginalFunc(pClient, pData, bufType);
}

bool FASTCALL Hook_SendNetMessage_ServerSideClient(CServerSideClientBase *pClient, CNetMessage *pData, NetChannelBufType_t bufType)
{
	return Hook_SendNetMessage(pClient, pData, bufType, g_pfnSendNetMessage_ServerSideClient);
}

bool FASTCALL Hook_SendNetMessage_HLTVClient(CServerSideClientBase *pClient, CNetMessage *pData, NetChannelBufType_t bufType)
{
	return Hook_SendNetMessage(pClient, pData, bufType, g_pfnSendNetMessage_HLTVClient);
}

// pMgrDoNotUse is named as such because the variable is optimized out in Windows builds and will not be passed to the function.
// The original Windows function just uses the global singleton instead.
void FASTCALL Hook_SetPendingHostStateRequest(CHostStateMgr* pMgrDoNotUse, CHostStateRequest *pRequest)
{
	// When IVEngineServer::ChangeLevel is called by the plugin or the server code,
	// (which happens at the end of a map), the server-defined addon does not change.
	// Also, host state requests coming from that function will always have "ChangeLevel" in its KV's name.
	// We can use this information to always be aware of what the original addon is.
	//
	// Upstream (4e15151/3e04eff): track a *loading* workshop map here and only commit it when
	// the level actually starts (SwitchToLoop). We commit in Hook_StartupServer instead.

	g_MultiAddonManager.SetLoadingWorkshopMap(g_MultiAddonManager.GetCurrentWorkshopMap().c_str());

	if (!pRequest->m_pKV)
	{
		// g_pEngineServer->IsMapValid takes into account mounted addons so we have to do this instead
		char szFileName[MAX_PATH];
		V_snprintf(szFileName, sizeof(szFileName), "maps/%s.vpk", pRequest->m_LevelName.Get());
		bool bValveMap = g_pFullFileSystem->FileExists(szFileName, "MOD");

		// Workshop map changes from end of match votes have null keyvalues
		// ...and when such votes lead to reloading the CURRENT map, m_Addons will also be null, in which case we want to keep the workshop map unchanged
		if (!pRequest->m_Addons.IsEmpty())
			g_MultiAddonManager.SetLoadingWorkshopMap(pRequest->m_Addons);
		else if (bValveMap) // Sadly this will include any workshop maps that share names with shipped Valve maps, but at this point there's no way to tell
			g_MultiAddonManager.ClearLoadingWorkshopMap();
	}
	else if (V_stricmp(pRequest->m_pKV->GetName(), "ChangeLevel"))
	{
		if (!V_stricmp(pRequest->m_pKV->GetName(), "map_workshop"))
			g_MultiAddonManager.SetLoadingWorkshopMap(pRequest->m_pKV->GetString("customgamemode", ""));
		else
			g_MultiAddonManager.ClearLoadingWorkshopMap();
	}

	// Valve changed the way community maps (like de_dogtown) are loaded
	// Now their content lives in addons and they're mounted internally somehow (m_Addons is already set to it by this point)
	// So check if the addon is indeed one of the community maps and keep it, otherwise clients would error out due to missing assets
	// Each map has its own folder under game/csgo_community_addons which is mounted as "OFFICIAL_ADDONS"
	if (!pRequest->m_Addons.IsEmpty() && g_pFullFileSystem->IsDirectory(pRequest->m_Addons.String(), "OFFICIAL_ADDONS"))
		g_MultiAddonManager.SetLoadingWorkshopMap(pRequest->m_Addons);

	if (g_MultiAddonManager.m_ExtraAddons.Count() == 0)
		return g_pfnSetPendingHostStateRequest(pMgrDoNotUse, pRequest);

	// Rebuild the addon list. We always start with the original addon.
	const std::string &loadingMap = g_MultiAddonManager.GetLoadingWorkshopMap();
	if (loadingMap.empty())
	{
		pRequest->m_Addons = VectorToString(g_MultiAddonManager.m_ExtraAddons).c_str();
	}
	else
	{
		// Don't add the same addon twice. Hopefully no server owner is diabolical enough to do things like `map de_dust2 customgamemode=1234,5678`.
		CUtlVector<std::string> newAddons;
		newAddons.CopyArray(g_MultiAddonManager.m_ExtraAddons.Base(), g_MultiAddonManager.m_ExtraAddons.Count());
		newAddons.FindAndRemove(loadingMap.c_str());
		newAddons.AddToHead(loadingMap.c_str());
		pRequest->m_Addons = VectorToString(newAddons).c_str();
	}

	g_pfnSetPendingHostStateRequest(pMgrDoNotUse, pRequest);
}

void MultiAddonManager::CheckClientAddons(uint64 steamID64)
{
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	clientInfo.connectedState = CLIENTCONN_JOINED;

	CUtlVector<std::string> addons;
	GetClientAddons(addons, steamID64);
	// We don't have an extra addon set so do nothing here, also don't do anything if we're a listenserver
	if (addons.Count() == 0 || !g_pEngineServer->IsDedicatedServer())
		return;

	if (!clientInfo.currentPendingAddon.empty())
	{
		if (Plat_FloatTime() - clientInfo.lastActiveTime > mm_extra_addons_timeout.Get())
		{
			if (mm_addon_debug.Get())
				Message("%s: Client %lli has reconnected after the timeout or did not receive the addon message, will not add addon %s to the downloaded list\n",
					__func__, steamID64, clientInfo.currentPendingAddon.c_str());
		}
		else
		{
			if (mm_addon_debug.Get())
				Message("%s: Client %lli has connected within the interval with the pending addon %s, will send next addon in SendNetMessage hook\n",
					__func__, steamID64, clientInfo.currentPendingAddon.c_str());

			clientInfo.downloadedAddons.AddToTail(clientInfo.currentPendingAddon);
		}
		// Reset the current pending addon anyway, SendNetMessage tells us which addon to download next.
		clientInfo.currentPendingAddon.clear();
	}
	clientInfo.lastActiveTime = Plat_FloatTime();
	return;
}

KHook::Return<bool> MultiAddonManager::Hook_ClientConnect(IServerGameClients*, CPlayerSlot slot, const char *pszName, uint64 steamID64, const char *pszNetworkID, bool unk1, CBufferString *pRejectReason )
{
	CheckClientAddons(steamID64);
	return { KHook::Action::Ignore };
}

KHook::Return<bool> MultiAddonManager::Hook_CanHLTVClientConnect(IServerGameClients*, int index, const CSteamID &steamID, int *pRejectReason)
{
	CheckClientAddons(steamID.ConvertToUint64());
	return { KHook::Action::Ignore };
}

KHook::Return<void> MultiAddonManager::Hook_ClientDisconnect(IServerGameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName, uint64 steamID64, const char *pszNetworkID )
{
	// Mark the disconnection time for caching purposes.
	g_ClientAddons[steamID64].lastActiveTime = Plat_FloatTime();
	g_ClientAddons[steamID64].connectedState = CLIENTCONN_NONE;
	return { KHook::Action::Ignore };
}

KHook::Return<void> MultiAddonManager::Hook_ClientActive(IServerGameClients*, CPlayerSlot slot, bool bLoadGame, const char * pszName, uint64 steamID64)
{
	// When the client reaches this stage, they should already have all the necessary addons downloaded, so we can safely remove the downloaded addons list here.
	if (!mm_cache_clients_with_addons.Get())
		g_ClientAddons[steamID64].downloadedAddons.RemoveAll();
	return { KHook::Action::Ignore };
}

KHook::Return<void> MultiAddonManager::Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick)
{
	static double s_flTime = 0.0f;

	if (m_bPendingSteamRefresh)
	{
		ISteamGameServer *pGameServer = SteamGameServer();
		const bool bLoggedOn = pGameServer && pGameServer->BLoggedOn();
		const bool bDelayElapsed = Plat_FloatTime() - m_flSteamApiActivatedTime >= mm_steam_refresh_delay.Get();

		if (bLoggedOn && bDelayElapsed && GetSteamUGC())
		{
			const bool bReloadMap = m_bPendingReloadMap;
			m_bPendingSteamRefresh = false;
			m_bPendingReloadMap = false;
			Message("Steam logon ready, refreshing addons\n");
			RefreshAddons(bReloadMap);
		}
	}

	// Print download progress and verify mounted addons every second
	if (Plat_FloatTime() - s_flTime > 1.f)
	{
		s_flTime = Plat_FloatTime();
		PrintDownloadProgress();
		CheckMountedAddons();
	}

	if (!m_TimedOutClients.size())
		return { KHook::Action::Ignore };

	auto pClients = GetClientList();

	FOR_EACH_VEC(*pClients, i)
	{
		auto pClient = (*pClients)[i];

		uint64 steamID64 = pClient->GetClientSteamID().ConvertToUint64();

		if (m_TimedOutClients.erase(steamID64))
		{
			pClient->Disconnect(NETWORK_DISCONNECT_TIMEDOUT, "Required Workshop addon download was not accepted in time");
			g_ClientAddons[steamID64].connectedState = CLIENTCONN_NONE;
		}
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> MultiAddonManager::Hook_PostEvent(IGameEventSystem*, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64 *clients,
	INetworkMessageInternal *pEvent, const CNetMessage *pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();

	if (mm_block_disconnect_messages.Get() && info->m_MessageId == GE_Source1LegacyGameEvent)
	{
		auto pMsg = pData->ToPB<CMsgSource1LegacyGameEvent>();

		static int sDisconnectId = g_pGameEventManager->LookupEventId("player_disconnect");

		if (pMsg->eventid() == sDisconnectId)
		{
			IGameEvent *pEvent = g_pGameEventManager->UnserializeEvent(*pMsg);

			// This will prevent "loop shutdown" messages in the chat when clients reconnect
			// As far as we're aware, there are no other cases where this reason is used
			if (pEvent->GetInt("reason") == NETWORK_DISCONNECT_LOOPSHUTDOWN)
				*(uint64*)clients = 0;
		}
	}

	return { KHook::Action::Ignore };
}

KHook::Return<int> MultiAddonManager::Hook_LoadEventsFromFile(IGameEventManager2* pThis, const char *filename, bool bSearchAll)
{
	if (!g_pGameEventManager && pThis)
		g_pGameEventManager = pThis;

	return { KHook::Action::Ignore };
}

void FASTCALL Hook_ReplyConnection(INetworkGameServer *server, CServerSideClient *client)
{
	uint64 steamID64 = client->GetClientSteamID().ConvertToUint64();
	// Clear cache if necessary.
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	if (mm_cache_clients_with_addons.Get() && mm_cache_clients_duration.Get() != 0 && Plat_FloatTime() - clientInfo.lastActiveTime > mm_cache_clients_duration.Get())
	{
		if (mm_addon_debug.Get())
			Message("%s: Client %lli has not connected for a while, clearing the cache\n", __func__, steamID64);

		clientInfo.currentPendingAddon.clear();
		clientInfo.downloadedAddons.RemoveAll();
	}
	clientInfo.lastActiveTime = Plat_FloatTime();

	// Server copies the CUtlString from CNetworkGameServer to this client.
	CUtlString *addons = (CUtlString *)((uintptr_t)server + g_iServerAddonsOffset);
	CUtlString originalAddons = *addons;

	// Figure out which addons the client should be loading.
	CUtlVector<std::string> clientAddons;
	g_MultiAddonManager.GetClientAddons(clientAddons, steamID64);
	if (clientAddons.Count() == 0)
	{
		// No addons to send. This means the list of original addons is empty as well.
		assert(originalAddons.IsEmpty());
		clientInfo.currentPendingAddon.clear();
		g_pfnReplyConnection(server, client);
		return;
	}

	if (clientInfo.connectedState != CLIENTCONN_CONNECTING)
	{
		clientInfo.connectionStartTime = Plat_FloatTime();
		clientInfo.connectedState = CLIENTCONN_CONNECTING;
	}
	else if (mm_addon_connection_timeout.Get() > 0 &&
		clientInfo.connectedState == CLIENTCONN_CONNECTING && 
		Plat_FloatTime() - clientInfo.connectionStartTime > mm_addon_connection_timeout.Get())
	{
		// Can't kick right now as this will crash on windows, so defer to the next frame
		g_MultiAddonManager.AddTimedOutClient(steamID64);
		return;
	}

	// Handle the first addon here. The rest should be handled in the SendNetMessage hook.
	if (clientInfo.downloadedAddons.Find(clientAddons[0]) == -1)
		clientInfo.currentPendingAddon = clientAddons[0];

	// In some cases, clients can do a signature check on addons which fails and instantly disconnects them
	// As a mitigation, remove all undownloaded addons so the client never does the failing signature check
	FOR_EACH_VEC_BACK(clientAddons, i)
	{
		if (clientInfo.downloadedAddons.Find(clientAddons[i]) != -1 || clientAddons[i] == clientInfo.currentPendingAddon)
			continue;

		clientAddons.Remove(i);
	}
	
	*addons = VectorToString(clientAddons).c_str();

	if (mm_addon_debug.Get())
		Message("%s: Sending addons %s to steamID64 %lli\n", __func__, addons->Get(), steamID64);

	g_pfnReplyConnection(server, client);

	*addons = originalAddons;
}

uint64 FASTCALL Hook_ScriptGetAddon()
{
	if (!g_MultiAddonManager.m_ExtraAddons.Count())
		return g_pfnScriptGetAddon();

	uint64 iAddon = V_StringToUint64(g_MultiAddonManager.GetCurrentWorkshopMap().c_str(), 0);
	
	if (!iAddon)
		return g_pfnScriptGetAddon();

	return iAddon;
}
