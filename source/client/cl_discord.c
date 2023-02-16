/*
Copyright (C) 2023 - Team Forbidden LLC.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "client.h"

#include "discord_register.h"
#include "discord_rpc.h"

#define DISCORD_APP_ID				1074038981802598491
#define G_STRINGIFY(macro_or_string)	G_STRINGIFY_ARG (macro_or_string)
#define	G_STRINGIFY_ARG(contents)	#contents

enum EDiscordResult {
	DiscordResult_Ok,
	DiscordResult_ServiceUnavailable,
	DiscordResult_InvalidVersion,
	DiscordResult_LockFailed,
	DiscordResult_InternalError,
	DiscordResult_InvalidPayload,
	DiscordResult_InvalidCommand,
	DiscordResult_InvalidPermissions,
	DiscordResult_NotFetched,
	DiscordResult_NotFound,
	DiscordResult_Conflict,
	DiscordResult_InvalidSecret,
	DiscordResult_InvalidJoinSecret,
	DiscordResult_NoEligibleActivity,
	DiscordResult_InvalidInvite,
	DiscordResult_NotAuthenticated,
	DiscordResult_InvalidAccessToken,
	DiscordResult_ApplicationMismatch,
	DiscordResult_InvalidDataUrl,
	DiscordResult_InvalidBase64,
	DiscordResult_NotFiltered,
	DiscordResult_LobbyFull,
	DiscordResult_InvalidLobbySecret,
	DiscordResult_InvalidFilename,
	DiscordResult_InvalidFileSize,
	DiscordResult_InvalidEntitlement,
	DiscordResult_NotInstalled,
	DiscordResult_NotRunning,
	DiscordResult_InsufficientBuffer,
	DiscordResult_PurchaseCanceled,
	DiscordResult_InvalidGuild,
	DiscordResult_InvalidEvent,
	DiscordResult_InvalidChannel,
	DiscordResult_InvalidOrigin,
	DiscordResult_RateLimited,
	DiscordResult_OAuth2Error,
	DiscordResult_SelectChannelTimeout,
	DiscordResult_GetGuildTimeout,
	DiscordResult_SelectVoiceForceRequired,
	DiscordResult_CaptureShortcutAlreadyListening,
	DiscordResult_UnauthorizedForAchievement,
	DiscordResult_InvalidGiftCode,
	DiscordResult_PurchaseError,
	DiscordResult_TransactionAborted
};

/*
* CL_DiscordErrorString
*/
static const char *CL_DiscordErrorString(enum EDiscordResult result) {

	switch (result) {
		case DiscordResult_Ok: return "Ok";
		case DiscordResult_ServiceUnavailable: return "Service Unavailable";
		case DiscordResult_InvalidVersion: return "Invalid Version";
		case DiscordResult_LockFailed: return "Lock Failed";
		case DiscordResult_InternalError: return "Internal Error";
		case DiscordResult_InvalidPayload: return "Invalid Payload";
		case DiscordResult_InvalidCommand: return "Invalid Command";
		case DiscordResult_InvalidPermissions: return "Invalid Permissions";
		case DiscordResult_NotFetched: return "Not Fetched";
		case DiscordResult_NotFound: return "Not Found";
		case DiscordResult_Conflict: return "Conflict";
		case DiscordResult_InvalidSecret: return "Invalid Secret";
		case DiscordResult_InvalidJoinSecret: return "Invalid Join Secret";
		case DiscordResult_NoEligibleActivity: return "No Eligible Activity";
		case DiscordResult_InvalidInvite: return "Invalid Invite";
		case DiscordResult_NotAuthenticated: return "Not Authenticated";
		case DiscordResult_InvalidAccessToken: return "Invalid Access Token";
		case DiscordResult_ApplicationMismatch: return "Application Mismatch";
		case DiscordResult_InvalidDataUrl: return "Invalid Data Url";
		case DiscordResult_InvalidBase64: return "Invalid Base64";
		case DiscordResult_NotFiltered: return "Not Filtered";
		case DiscordResult_LobbyFull: return "Lobby Full";
		case DiscordResult_InvalidLobbySecret: return "Invalid Lobby Secret";
		case DiscordResult_InvalidFilename: return "Invalid Filename";
		case DiscordResult_InvalidFileSize: return "Invalid File Size";
		case DiscordResult_InvalidEntitlement: return "Invalid Entitlement";
		case DiscordResult_NotInstalled: return "Not Installed";
		case DiscordResult_NotRunning: return "Not Running";
		case DiscordResult_InsufficientBuffer: return "Insufficient Buffer";
		case DiscordResult_PurchaseCanceled: return "Purchase Canceled";
		case DiscordResult_InvalidGuild: return "Invalid Guild";
		case DiscordResult_InvalidEvent: return "Invalid Event";
		case DiscordResult_InvalidChannel: return "Invalid Channel";
		case DiscordResult_InvalidOrigin: return "Invalid Origin";
		case DiscordResult_RateLimited: return "Rate Limited";
		case DiscordResult_OAuth2Error: return "OAuth2 Error";
		case DiscordResult_SelectChannelTimeout: return "Select Channel Timeout";
		case DiscordResult_GetGuildTimeout: return "Get Guild Timeout";
		case DiscordResult_SelectVoiceForceRequired: return "Select Voice Force Required";
		case DiscordResult_CaptureShortcutAlreadyListening: return "Capture Shortcut Already Listening";
		case DiscordResult_UnauthorizedForAchievement: return "Unauthorized For Achievement";
		case DiscordResult_InvalidGiftCode: return "Invalid Gift Code";
		case DiscordResult_PurchaseError: return "Purchase Error";
		case DiscordResult_TransactionAborted: return "Transaction Aborted";
	}

	return "Unknown Error ID";
}

/*
* CL_DiscordDisconnected
*/
static void CL_DiscordDisconnected(int errorCode, const char* message) {
	Com_Printf(S_COLOR_RED "Discord error %s: %s\n", CL_DiscordErrorString(errorCode), message);
}

typedef enum {
	DISCORD_INVALID,
	DISCORD_INACTIVE,
	DISCORD_ACTIVE
} cl_discord_status_t;

typedef struct {
	_Bool initialized;
	cl_discord_status_t status;
} cl_discord_state_t;

static cl_discord_state_t cl_discord_state;

/*
* CL_DiscordReady
*/
static void CL_DiscordReady(const DiscordUser *user) {
    Com_Printf("Loading Discord module... (%s)\n", user->username);
	cl_discord_state.initialized = true;
}

/*
* CL_UpdateDiscord
*/

void CL_UpdateDiscord(void) {

	if (cl_discord_state.initialized) {
		DiscordRichPresence presence = { 0 };
		_Bool needs_update = false;

		char details[128];
		char joinSecret[128];
		char spectateSecret[128];

		if (cls.state == CA_CONNECTED) {
			if (cl_discord_state.status != DISCORD_ACTIVE) {
				needs_update = true;

                const char *valid_maps[] = { "wfamphi1", "[wfbomb]", "[wfca]", "[wfctf]", "[wfda]", "[wfdm]", "wfrace1", "wftutorial1" };
                const size_t num_valid_maps = sizeof( valid_maps ) / sizeof( valid_maps[0] );
                const char *mapname = cl.configstrings[CS_MAPNAME];
                bool valid_map = false;
                for( size_t i = 0; i < num_valid_maps; i++ ) {
                  if( !Q_stricmp( mapname, valid_maps[i] ) ) {
                    valid_map = true;
                    break;
                  }
                }

                presence.largeImageKey = valid_map ? mapname : "default";

				presence.state = "Playing";

				Q_snprintfz(details, sizeof(details), "%s - %s", cl.configstrings[CS_GAMETYPENAME], cl.configstrings[CS_MAPNAME]);
				presence.details = details;

				if (cls.servertype != SOCKET_LOOPBACK) {

					presence.partyId = cls.servername; 

					Q_snprintfz(joinSecret, sizeof(joinSecret), "JOIN_%s", presence.partyId);
					presence.joinSecret = joinSecret;

					Q_snprintfz(spectateSecret, sizeof(spectateSecret), "SPCT_%s", presence.partyId);
					presence.spectateSecret = spectateSecret;
				}

				presence.partySize = cl.snapShots[cl.currentSnapNum & UPDATE_MASK].numplayers;
                presence.partyMax = atoi(cl.configstrings[CS_MAXCLIENTS]);
				cl_discord_state.status = DISCORD_ACTIVE;
				presence.instance = true;
			}
		} else {
			if (cl_discord_state.status != DISCORD_INACTIVE) {
				needs_update = true;
			
				presence.largeImageKey = "mainmenu";
				presence.state = "Main Menu";

				cl_discord_state.status = DISCORD_INACTIVE;
			}
		}

		if (needs_update) {
			Discord_UpdatePresence(&presence);
		}
	}

	Discord_RunCallbacks();
}

/*
* CL_DiscordJoinGame
*/
static void CL_DiscordJoinGame(const char *secret) {

	if (strncasecmp(secret, "JOIN_", 5)) { // g_ascii_strncasecmp 
		Com_Printf(S_COLOR_YELLOW "Invalid invitation\n");
		return;
	}

}

/*
* CL_DiscordJoinRequest
*/
static void CL_DiscordJoinRequest(const DiscordUser *request) {

	// TODO: display UI for this
	Discord_Respond(request->userId, DISCORD_REPLY_YES);
}

/*
* CL_DiscordSpectateGame
*/
static void CL_DiscordSpectateGame(const char *secret) {

	if (strncasecmp(secret, "SPCT_", 5)) {
		Com_Printf(S_COLOR_YELLOW "Invalid invitation\n");
		return;
	}

}

/*
* CL_InitDiscord
*/
void CL_InitDiscord( void )
{
	static DiscordEventHandlers handlers = {
		.ready = CL_DiscordReady,
		.disconnected = CL_DiscordDisconnected,
		.joinGame = CL_DiscordJoinGame,
		.joinRequest = CL_DiscordJoinRequest,
		.errored = CL_DiscordDisconnected,
		.spectateGame = CL_DiscordSpectateGame
	};

	Discord_Initialize(G_STRINGIFY(DISCORD_APP_ID), &handlers, 1, NULL);	
}

/*
* CL_ShutdownDiscord
*/
void CL_ShutdownDiscord( void )
{
	Discord_Shutdown();

	memset(&cl_discord_state, 0, sizeof(cl_discord_state));

    Com_Printf( "Discord module unloaded.\n" );			
}
