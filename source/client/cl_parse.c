/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// cl_parse.c  -- parse a message received from the server

#include "client.h"
#include "tracy/TracyC.h"

static void CL_InitServerDownload( const char *filename, int size, unsigned checksum, bool allow_localhttpdownload,
							const char *url, bool initial, int dlId, int ackchunks );
void CL_StopServerDownload( void );

//=============================================================================

/*
* CL_CanDownloadModules
* 
* The user has to give permission for modules to be downloaded
*/
bool CL_CanDownloadModules( void )
{
#if 0
	if( !Q_stricmp( FS_GameDirectory(), FS_BaseGameDirectory() ) )
	{
		Com_Error( ERR_DROP, "Can not download modules to the base directory" );
		return false;
	}
#endif
	if( !cl_download_allow_modules->integer )
	{
		Com_Error( ERR_DROP, "Downloading of modules disabled." );
		return false;
	}

	return true;
}

/*
* CL_DownloadRequest
* 
* Request file download
* return false if couldn't request it for some reason
* Files with .pk3 or .pak extension have to have gamedir attached
* Other files must not have gamedir
*/
bool CL_DownloadRequest( const char *filename, bool requestpak )
{
	if( cls.download.requestname )
	{
		Com_Printf( "Can't download: %s. Download already in progress.\n", filename );
		return false;
	}

	if( !COM_ValidateRelativeFilename( filename ) )
	{
		Com_Printf( "Can't download: %s. Invalid filename.\n", filename );
		return false;
	}

	if( FS_CheckPakExtension( filename ) )
	{
		if( FS_PakFileExists( filename ) )
		{
			Com_Printf( "Can't download: %s. File already exists.\n", filename );
			return false;
		}

		if( !Q_strnicmp( COM_FileBase( filename ), "modules", strlen( "modules" ) ) )
		{
			if( !CL_CanDownloadModules() )
				return false;
		}
	}
	else
	{
		if( FS_FOpenFile( filename, NULL, FS_READ ) != -1 )
		{
			Com_Printf( "Can't download: %s. File already exists.\n", filename );
			return false;
		}

		if( !requestpak ) {
			const char *extension;

			// only allow demo downloads
			extension = COM_FileExtension( filename );
			if( !extension || Q_stricmp( extension, APP_DEMO_EXTENSION_STR ) )
			{
				Com_Printf( "Can't download, got arbitrary file type: %s\n", filename );
				return false;
			}
		}
	}

	if( cls.socket->type == SOCKET_LOOPBACK )
	{
		Com_DPrintf( "Can't download: %s. Loopback server.\n", filename );
		return false;
	}

	Com_Printf( "Asking to download: %s\n", filename );

	cls.download.requestpak = requestpak;
	cls.download.requestname = Mem_ZoneMalloc( sizeof( char ) * ( strlen( filename ) + 1 ) );
	Q_strncpyz( cls.download.requestname, filename, sizeof( char ) * ( strlen( filename ) + 1 ) );
	cls.download.timeout = Sys_Milliseconds() + 5000;
	CL_AddReliableCommand( va( "download %i \"%s\"", requestpak, filename ) );

	return true;
}

/*
* CL_CheckOrDownloadFile
* 
* Returns true if the file exists or couldn't send download request
* Files with .pk3 or .pak extension have to have gamedir attached
* Other files must not have gamedir
*/
bool CL_CheckOrDownloadFile( const char *filename )
{
	const char *ext;

	if( !cl_downloads->integer )
		return true;

	if( !COM_ValidateRelativeFilename( filename ) )
		return true;

	ext = COM_FileExtension( filename );
	if( !ext )
		return true;

	if( FS_CheckPakExtension( filename ) )
	{
		if( FS_PakFileExists( filename ) )
			return true;
	}
	else
	{
		if( FS_FOpenFile( filename, NULL, FS_READ ) != -1 )
			return true;
	}

	if( !CL_DownloadRequest( filename, true ) )
		return true;

	cls.download.requestnext = true; // call CL_RequestNextDownload when done

	return false;
}

/*
* CL_DownloadComplete
* 
* Checks downloaded file's checksum, renames it and adds to the filesystem.
*/
static void CL_DownloadComplete( void )
{
	unsigned checksum = 0;
	int length;

	FS_FCloseFile( cls.download.filenum );
	cls.download.filenum = 0;

	// verify checksum
	if( FS_CheckPakExtension( cls.download.name ) )
	{
		if( !FS_IsPakValid( cls.download.tempname, &checksum ) )
		{
			Com_Printf( "Downloaded file is not a valid pack file. Removing\n" );
			FS_RemoveBaseFile( cls.download.tempname );
			return;
		}
	}
	else
	{
		length = FS_LoadBaseFile( cls.download.tempname, NULL, NULL, 0 );
		if( length < 0 )
		{
			Com_Printf( "Error: Couldn't load downloaded file\n" );
			return;
		}
		checksum = FS_ChecksumBaseFile( cls.download.tempname, false );
	}

	if( cls.download.checksum != checksum )
	{
		Com_Printf( "Downloaded file has wrong checksum. Removing: %u %u %s\n", cls.download.checksum, checksum, cls.download.tempname );
		FS_RemoveBaseFile( cls.download.tempname );
		return;
	}

	if( !FS_MoveBaseFile( cls.download.tempname, cls.download.name ) )
	{
		Com_Printf( "Failed to rename the downloaded file\n" );
		return;
	}

	// Maplist hook so we also know when a new map is added
	if( FS_CheckPakExtension( cls.download.name ) ) {
		ML_Update();
	}

	cls.download.successCount++;
	cls.download.timeout = 0;
}

/*
* CL_FreeDownloadList
*/
void CL_FreeDownloadList( void )
{
	download_list_t *next;

	while( cls.download.list )
	{
		next = cls.download.list->next;
		Mem_ZoneFree( cls.download.list->filename );
		Mem_ZoneFree( cls.download.list );
		cls.download.list = next;
	}
}

/*
* CL_DownloadDone
*/
void CL_DownloadDone( void )
{
	bool requestnext;

	if( cls.download.name )
		CL_StopServerDownload();

	Mem_ZoneFree( cls.download.requestname );
	cls.download.requestname = NULL;

	requestnext = cls.download.requestnext;
	cls.download.requestnext = false;
	cls.download.requestpak = false;
	cls.download.timeout = 0;
	cls.download.timestart = 0;
	cls.download.offset = cls.download.baseoffset = 0;
	cls.download.web = false;
	cls.download.filenum = 0;
	cls.download.cancelled = false;

	// the server has changed map during the download
	if( cls.download.pending_reconnect )
	{
		cls.download.pending_reconnect = false;
		CL_FreeDownloadList();
		CL_ServerReconnect_f();
		return;
	}

	if( requestnext && cls.state > CA_DISCONNECTED )
		CL_RequestNextDownload();
}

/*
* CL_WebDownloadDoneCb
*/
static void CL_WebDownloadDoneCb( int status, const char *contentType, void *privatep )
{
	download_t download = cls.download;
	bool disconnect = download.disconnect;
	bool cancelled = download.cancelled;
	bool success = (download.offset == download.size) && (status > -1);

	Com_Printf( "Web download %s: %s (%i)\n", success ? "successful" : "failed", download.tempname, status );

	if( success ) {
		CL_DownloadComplete();
	}
	if( cancelled ) {
		cls.download.requestnext = false;
	}

	// check if user pressed escape to stop the download
	if( disconnect ) {
		CL_Disconnect( NULL ); // this also calls CL_DownloadDone()
		return;
	}

	CL_DownloadDone();
}

/*
* CL_WebDownloadReadCb
*/
static size_t CL_WebDownloadReadCb( const void *buf, size_t numb, float percentage, int status,
	const char *contentType, void *privatep )
{
	bool stop = cls.download.disconnect || cls.download.cancelled || status < 0 || status >= 300;
	size_t write = 0;

	if( !stop ) {
		write = FS_Write( buf, numb, cls.download.filenum );
	}

	// ignore percentage passed by the downloader as it doesn't account for total file size
	// of resumed downloads
	cls.download.offset += write;
	cls.download.percent = (double)cls.download.offset / (double)cls.download.size;
	clamp( cls.download.percent, 0, 1 );

	Cvar_ForceSet( "cl_download_percent", va( "%.1f", cls.download.percent * 100 ) );

	cls.download.timeout = 0;

	// abort if disconnected, canclled or writing failed
	return stop ? !numb : write;
}

/*
* CL_CollapseUrlSlashes
*
* Collapses runs of '/' in the path of an url, in place. The "//" of the scheme and
* everything from the query delimiter on are left alone.
*
* Only touches urls that carry a scheme: without one there's no way to tell a path from
* a scheme-relative "//host/path", whose leading "//" must survive.
*/
static char *CL_CollapseUrlSlashes( char *url )
{
	char *src, *dst;

	src = strstr( url, "://" );
	if( !src )
		return url;
	src += 3;
	dst = src;

	while( *src && *src != '?' && *src != '#' ) {
		*dst++ = *src++;
		if( src[-1] == '/' ) {
			while( *src == '/' )
				src++;
		}
	}

	if( dst != src )
		memmove( dst, src, strlen( src ) + 1 );

	return url;
}

/*
* CL_SendDownloadAck
*
* Reports what we hold: the first chunk we are still missing, and a bitset naming the chunks
* above it that already arrived. This is absolute state rather than a delta, so it is safe to
* lose, to reorder and to repeat - which is the whole reason the download survives a transport
* that does any of those.
*
* It goes out as its own packet rather than riding the next message to the server: piggybacking
* would mean waiting on CL_SendMessagesToServer's 100ms connecting-state gate, which on its own
* would cap the window from refilling more than ten times a second.
*/
static void CL_SendDownloadAck( void )
{
	uint8_t data[64];
	msg_t msg;
	uint64_t bits[2] = { 0, 0 };
	int i;

	// the netchan is only set up from CA_CONNECTED on, and a download parsed out of a demo is
	// ignored - so neither ever has an ack to send
	if( cls.state < CA_CONNECTED || cls.demo.playing )
		return;

	cls.download.lastAckChunk = cls.download.baseChunk;
	cls.download.ackFlushTime = 0;

	// bit i names chunk baseChunk+1+i. baseChunk itself is by definition the one we are missing
	for( i = 0; i < DOWNLOAD_MAX_WINDOW; i++ )
	{
		size_t c = cls.download.baseChunk + 1 + i;

		if( DL_BitGet( cls.download.bits, c & DOWNLOAD_ACK_MASK ) )
			DL_BitSet( bits, i );
	}

	MSG_Init( &msg, data, sizeof( data ) );
	MSG_Clear( &msg );

	MSG_WriteByte( &msg, clc_dlack );
	MSG_WriteByte( &msg, cls.download.dlId );
	MSG_WriteLong( &msg, (int)cls.download.baseChunk );
	MSG_WriteLong( &msg, (int)(uint32_t)bits[0] );
	MSG_WriteLong( &msg, (int)(uint32_t)( bits[0] >> 32 ) );
	MSG_WriteLong( &msg, (int)(uint32_t)bits[1] );
	MSG_WriteLong( &msg, (int)(uint32_t)( bits[1] >> 32 ) );

	// the lane has to match whatever the rest of this connection's messages use: everything sent
	// while connecting goes reliable and everything sent while spawned goes unreliable, and the
	// netchan drops anything that arrives out of sequence, so mixing lanes would lose packets
	CL_Netchan_Transmit( &msg, cls.state < CA_ACTIVE ? NET_SEND_RELIABLE : NET_SEND_UNRELIABLE );
}

/*
* CL_InitServerDownload
*
* Handles server's initdownload message, starts web or server download if possible
*/
static void CL_InitServerDownload( const char *filename, int size, unsigned checksum, bool allow_localhttpdownload,
							  const char *url, bool initial, int dlId, int ackchunks )
{
	int alloc_size;
	const char *baseurl;
	download_list_t *dl;

	// ignore download commands coming from demo files
	if( cls.demo.playing )
		return;

	if( !cls.download.requestname )
	{
		Com_Printf( "Got init download message without request\n" );
		return;
	}

	if( cls.download.filenum || cls.download.web )
	{
		Com_Printf( "Got init download message while already downloading\n" );
		return;
	}

	if( size == -1 )
	{
		// means that download was refused
		Com_Printf( "Server refused download request: %s\n", url ); // if it's refused, url field holds the reason
		CL_DownloadDone();
		return;
	}

	if( size <= 0 )
	{
		Com_Printf( "Server gave invalid size, not downloading\n" );
		CL_DownloadDone();
		return;
	}

	if( checksum == 0 )
	{
		Com_Printf( "Server didn't provide checksum, not downloading\n" );
		CL_DownloadDone();
		return;
	}

	if( !COM_ValidateRelativeFilename( filename ) )
	{
		Com_Printf( "Not downloading, invalid filename: %s\n", filename );
		CL_DownloadDone();
		return;
	}

	if( FS_CheckPakExtension( filename ) != cls.download.requestpak )
	{
		const char *requested = cls.download.requestpak ? "pak" : "normal";
		const char *got = cls.download.requestpak ? "normal" : "pak";
		Com_Printf( "Got a %s file when requesting a %s file, not downloading\n", got, requested );
		CL_DownloadDone();
		return;
	}

	if( !strchr( filename, '/' ) )
	{
		Com_Printf( "Refusing to download file with no gamedir: %s\n", filename );
		CL_DownloadDone();
		return;
	}

	// check that it is in game or basegame dir
	if( strlen( filename ) < strlen( FS_GameDirectory() )+1 ||
		strncmp( filename, FS_GameDirectory(), strlen( FS_GameDirectory() ) ) ||
		filename[strlen( FS_GameDirectory() )] != '/' )
	{
		if( strlen( filename ) < strlen( FS_BaseGameDirectory() )+1 ||
			strncmp( filename, FS_BaseGameDirectory(), strlen( FS_BaseGameDirectory() ) ) ||
			filename[strlen( FS_BaseGameDirectory() )] != '/' )
		{
			Com_Printf( "Can't download, invalid game directory: %s\n", filename );
			CL_DownloadDone();
			return;
		}
	}

	if( FS_CheckPakExtension( filename ) )
	{
		if( strchr( strchr( filename, '/' ) + 1, '/' ) )
		{
			Com_Printf( "Refusing to download pack file to subdirectory: %s\n", filename );
			CL_DownloadDone();
			return;
		}

		// game modules and explicitly pure paks were only ever fetched from the official
		// update host. That host is gone, so there is no trusted source for them and they
		// must never be taken from a server-supplied mirror.
		if( !Q_strnicmp( COM_FileBase( filename ), "modules", strlen( "modules" ) ) )
		{
			Com_Printf( "Refusing to download game modules: %s\n", filename );
			CL_DownloadDone();
			return;
		}

		if( FS_IsExplicitPurePak( filename, NULL ) )
		{
			Com_Printf( "Refusing to download pure pack file: %s\n", filename );
			CL_DownloadDone();
			return;
		}

		if( FS_PakFileExists( filename ) )
		{
			Com_Printf( "Can't download, file already exists: %s\n", filename );
			CL_DownloadDone();
			return;
		}
	}
	else
	{
		if( strcmp( cls.download.requestname, strchr( filename, '/' ) + 1 ) )
		{
			Com_Printf( "Can't download, got different file than requested: %s\n", filename );
			CL_DownloadDone();
			return;
		}
	}

	if( initial )
	{
		if( cls.download.requestnext )
		{
			dl = cls.download.list;
			while( dl != NULL )
			{
				if( !Q_stricmp( dl->filename, filename ) )
				{
					Com_Printf( "Skipping, already tried downloading: %s\n", filename );
					CL_DownloadDone();
					return;
				}
				dl = dl->next;
			}
		}
	}

	alloc_size = strlen( "downloads" ) + 1 /* '/' */ + strlen( filename ) + 1;
	cls.download.name = Mem_ZoneMalloc( alloc_size );
	if( !cls.download.requestpak ) {
		// if we're not downloading a pak, this must be a demo so drop it into the gamedir
		Q_snprintfz( cls.download.name, alloc_size, "%s", filename );
	}
	else {
		if( FS_DownloadsDirectory() == NULL ) {
			Com_Printf( "Can't download, downloads directory is disabled\n" );
			CL_DownloadDone();
			return;
		}
		Q_snprintfz( cls.download.name, alloc_size, "%s/%s", "downloads", filename );
	}

	alloc_size = strlen( cls.download.name ) + strlen( ".tmp" ) + 1;
	cls.download.tempname = Mem_ZoneMalloc( alloc_size );
	Q_snprintfz( cls.download.tempname, alloc_size, "%s.tmp", cls.download.name );

	cls.download.origname = ZoneCopyString( filename );
	cls.download.web = false;
	cls.download.web_url = ZoneCopyString( url );
	cls.download.web_local_http = allow_localhttpdownload;
	cls.download.cancelled = false;
	cls.download.disconnect = false;
	cls.download.size = size;
	cls.download.checksum = checksum;
	cls.download.percent = 0;
	cls.download.timeout = 0;
	cls.download.retries = 0;
	cls.download.timestart = Sys_Milliseconds();
	cls.download.offset = 0;
	cls.download.baseoffset = 0;
	cls.download.pending_reconnect = false;
	cls.download.dlId = dlId;
	cls.download.numChunks = ( (size_t)size + DOWNLOAD_CHUNK_SIZE - 1 ) / DOWNLOAD_CHUNK_SIZE;
	cls.download.baseChunk = 0;
	cls.download.bytesReceived = 0;
	cls.download.resumeSkip = 0;
	cls.download.bits[0] = cls.download.bits[1] = 0;
	cls.download.ackChunks = ackchunks > 0 ? (size_t)ackchunks : 8;
	cls.download.lastAckChunk = 0;
	cls.download.ackFlushTime = 0;

	Cvar_ForceSet( "cl_download_name", COM_FileBase( filename ) );
	Cvar_ForceSet( "cl_download_percent", "0" );

	if( initial )
	{
		if( cls.download.requestnext )
		{
			dl = Mem_ZoneMalloc( sizeof( download_list_t ) );
			dl->filename = ZoneCopyString( filename );
			dl->next = cls.download.list;
			cls.download.list = dl;
		}
	}

	baseurl = cls.httpbaseurl;

	if( cl_downloads_from_web->integer && allow_localhttpdownload && url && url[0] != 0 ) {
		cls.download.web = true;
		Com_Printf( "Web download: %s from %s/%s\n", cls.download.tempname, baseurl, url );
	}
	else if( cl_downloads_from_web->integer && url && url[0] != 0 ) {
		cls.download.web = true;
		Com_Printf( "Web download: %s from %s\n", cls.download.tempname, url );
	}
	else {
		Com_Printf( "Server download: %s\n", cls.download.tempname );
	}

	cls.download.baseoffset = cls.download.offset = FS_FOpenBaseFile( cls.download.tempname, &cls.download.filenum, FS_APPEND );
	if( !cls.download.filenum )
	{
		Com_Printf( "Can't download, couldn't open %s for writing\n", cls.download.tempname );
		CL_DownloadDone();
		return;
	}

	if( cls.download.web ) {
		char *referer, *fullurl;
		const char *headers[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };

		if( cls.download.offset == cls.download.size ) {
			// special case for completed downloads to avoid passing empty HTTP range
			CL_WebDownloadDoneCb( 200, "", NULL );
			return;
		}

		alloc_size = strlen( APP_URI_SCHEME ) + strlen( NET_AddressToString( &cls.serveraddress ) ) + 1;
		referer = alloca( alloc_size );
		Q_snprintfz( referer, alloc_size, APP_URI_SCHEME "%s", NET_AddressToString( &cls.serveraddress ) );
		Q_strlwr( referer );

		if( allow_localhttpdownload ) {
			alloc_size = strlen( baseurl ) + 1 + strlen( url ) + 1;
			fullurl = alloca( alloc_size );
			Q_snprintfz( fullurl, alloc_size, "%s/%s", baseurl, url );
		}
		else {
			size_t url_len = strlen( url );
			alloc_size = url_len + 1 + strlen( filename ) * 3 + 1;
			fullurl = alloca( alloc_size );
			Q_snprintfz( fullurl, alloc_size, "%s/", url );
			Q_urlencode_unsafechars( filename, fullurl + url_len + 1, alloc_size - url_len - 1 );
		}

		// either base url may or may not have come with a trailing slash, and we've just
		// joined another one onto it. must happen before the session headers below, which
		// match fullurl against cls.httpbaseurl to decide whether to attach them
		CL_CollapseUrlSlashes( fullurl );

		headers[0] = "Referer";
		headers[1] = referer;

		CL_AddSessionHttpRequestHeaders( fullurl, &headers[2] );

		CL_AsyncStreamRequest( fullurl, headers, cl_downloads_from_web_timeout->integer / 100, cls.download.offset,
			CL_WebDownloadReadCb, CL_WebDownloadDoneCb, NULL, NULL, false );

		return;
	}

	// we may already hold part of the file. chunks are counted from the start of the file, so
	// resume at the last whole chunk on disk - and remember the bytes of the chunk after it that
	// are already there, because the file can only be appended to and that chunk will arrive in
	// full. this is the one place resumeSkip is ever non-zero
	cls.download.baseChunk = cls.download.offset / DOWNLOAD_CHUNK_SIZE;
	cls.download.resumeSkip = cls.download.offset % DOWNLOAD_CHUNK_SIZE;
	cls.download.bytesReceived = cls.download.offset;
	cls.download.lastAckChunk = cls.download.baseChunk;

	if( cls.download.offset >= cls.download.size || cls.download.baseChunk >= cls.download.numChunks )
	{
		// nothing left to fetch, the temp file is already the whole thing
		Com_Printf( "Download complete: %s\n", cls.download.name );
		CL_DownloadComplete();
		CL_AddReliableCommand( va( "nextdl \"%s\" %i", cls.download.origname, -1 ) );
		CL_DownloadDone();
		return;
	}

	// chunks past a hole are held here until the base catches up with them
	cls.download.reorder = Mem_ZoneMalloc( DOWNLOAD_ACK_BITS * DOWNLOAD_CHUNK_SIZE );

	cls.download.timeout = Sys_Milliseconds() + 3000;
	cls.download.retries = 0;

	// arms the server's pump at our resume point. always chunk aligned, even when the partial
	// file on disk is not
	CL_AddReliableCommand( va( "nextdl \"%s\" %i", cls.download.origname,
		(int)( cls.download.baseChunk * DOWNLOAD_CHUNK_SIZE ) ) );
	CL_SendMessagesToServer( true );
}

/*
* CL_InitDownload_f
*/
static void CL_InitDownload_f( void )
{
	const char *filename;
	const char *url;
	int size;
	unsigned checksum;
	bool allow_localhttpdownload, local_http;
	int dlId, ackchunks;

	// ignore download commands coming from demo files
	if( cls.demo.playing )
		return;

	// read the data
	filename = Cmd_Argv( 1 );
	size = atoi( Cmd_Argv( 2 ) );
	checksum = strtoul( Cmd_Argv( 3 ), NULL, 10 );
	local_http = ( atoi( Cmd_Argv( 4 ) ) != 0 );
	allow_localhttpdownload = local_http && cls.httpbaseurl != NULL;
	url = Cmd_Argv( 5 );
	// the download generation, and how far our base has to move before we report again
	dlId = atoi( Cmd_Argv( 6 ) );
	ackchunks = atoi( Cmd_Argv( 7 ) );

	if( local_http && !allow_localhttpdownload )
		url = "";

	CL_InitServerDownload( filename, size, checksum, allow_localhttpdownload, url, true, dlId, ackchunks );
}


/*
* CL_StopServerDownload
*/
void CL_StopServerDownload( void )
{
	if( cls.download.filenum > 0 ) {
		FS_FCloseFile( cls.download.filenum );
		cls.download.filenum = 0;
	}

	if( cls.download.cancelled ) {
		FS_RemoveBaseFile( cls.download.tempname );
	}

	Mem_ZoneFree( cls.download.name );
	cls.download.name = NULL;

	Mem_ZoneFree( cls.download.tempname );
	cls.download.tempname = NULL;

	Mem_ZoneFree( cls.download.origname );
	cls.download.origname = NULL;

	Mem_ZoneFree( cls.download.web_url );
	cls.download.web_url = NULL;

	Mem_ZoneFree( cls.download.reorder );
	cls.download.reorder = NULL;

	cls.download.offset = 0;
	cls.download.size = 0;
	cls.download.percent = 0;
	cls.download.timeout = 0;
	cls.download.retries = 0;
	cls.download.web = false;
	cls.download.dlId = 0;
	cls.download.numChunks = 0;
	cls.download.baseChunk = 0;
	cls.download.bytesReceived = 0;
	cls.download.resumeSkip = 0;
	cls.download.bits[0] = cls.download.bits[1] = 0;
	cls.download.ackFlushTime = 0;

	Cvar_ForceSet( "cl_download_name", "" );
	Cvar_ForceSet( "cl_download_percent", "0" );
}

/*
* CL_RetryDownload
* Resends download request
* Also aborts download if we have retried too many times
*/
static void CL_RetryDownload( void )
{
	if( ++cls.download.retries > 5 )
	{
		Com_Printf( "Download timed out: %s\n", cls.download.name );

		// let the server know we're done
		CL_AddReliableCommand( va( "nextdl \"%s\" %i", cls.download.origname, -2 ) );
		CL_DownloadDone();
	}
	else
	{
		cls.download.timeout = Sys_Milliseconds() + 3000;

		// repeating the ack unchanged is how the server is told nothing is arriving: with no new
		// information in it, it queues every chunk it has sent and we have not confirmed
		CL_SendDownloadAck();
	}
}

/*
* CL_CheckDownloadTimeout
* Retry downloading if too much time has passed since last download packet was received
*/
void CL_CheckDownloadTimeout( void )
{
	// a partial window that never accumulates a full ack interval - the tail of the file, or a
	// hole the server has already filled around - would otherwise sit until the 3 second retry
	if( cls.download.ackFlushTime && cls.download.ackFlushTime <= Sys_Milliseconds() )
		CL_SendDownloadAck();

	if( !cls.download.timeout || cls.download.timeout > Sys_Milliseconds() )
		return;

	if( cls.download.filenum )
	{
		CL_RetryDownload();
	}
	else
	{
		Com_Printf( "Download request timed out.\n" );
		CL_DownloadDone();
	}
}

/*
* CL_DownloadStatus_f
*/
void CL_DownloadStatus_f( void )
{
	if( !cls.download.requestname )
	{
		Com_Printf( "No download active\n" );
		return;
	}

	if( !cls.download.name )
	{
		Com_Printf( "%s: Requesting\n", COM_FileBase( cls.download.requestname ) );
		return;
	}

	Com_Printf( "%s: %s download %3.2f%c done\n", COM_FileBase( cls.download.name ),
		( cls.download.web ? "Web" : "Server" ), cls.download.percent * 100.0f, '%' );
}

/*
* CL_DownloadCancel_f
*/
void CL_DownloadCancel_f( void )
{
	if( !cls.download.requestname )
	{
		Com_Printf( "No download active\n" );
		return;
	}

	if( !cls.download.name )
	{
		CL_DownloadDone();
		Com_Printf( "Canceled download request\n" );
		return;
	}

	Com_Printf( "Canceled download of %s\n", cls.download.name );

	cls.download.cancelled = true;

	if( !cls.download.web ) {
		CL_AddReliableCommand( va( "nextdl \"%s\" %i", cls.download.origname, -2 ) ); // let the server know we're done
		CL_DownloadDone();
	}
}

/*
* CL_DownloadFlushChunk / CL_DownloadDrainReorder
*
* Writes chunk baseChunk and then everything the reorder ring already holds behind it. The temp
* file is opened FS_APPEND, so this is the only way bytes ever reach it: strictly in order, one
* chunk at a time, however scrambled the arrivals were.
*/
static void CL_DownloadFlushChunk( const uint8_t *data, size_t len )
{
	FS_Write( data + cls.download.resumeSkip, len - cls.download.resumeSkip, cls.download.filenum );
	cls.download.resumeSkip = 0;
	cls.download.baseChunk++;
	cls.download.offset = cls.download.baseChunk * DOWNLOAD_CHUNK_SIZE;
	if( cls.download.offset > cls.download.size )
		cls.download.offset = cls.download.size;
}

static void CL_DownloadDrainReorder( void )
{
	while( cls.download.baseChunk < cls.download.numChunks
		&& DL_BitGet( cls.download.bits, cls.download.baseChunk & DOWNLOAD_ACK_MASK ) )
	{
		size_t c = cls.download.baseChunk;
		size_t len = cls.download.size - c * DOWNLOAD_CHUNK_SIZE;

		if( len > DOWNLOAD_CHUNK_SIZE )
			len = DOWNLOAD_CHUNK_SIZE;

		DL_BitClear( cls.download.bits, c & DOWNLOAD_ACK_MASK );
		CL_DownloadFlushChunk( cls.download.reorder + ( c & DOWNLOAD_ACK_MASK ) * DOWNLOAD_CHUNK_SIZE, len );
	}
}

/*
* CL_ParseDownload
* Handles one chunk of the file from the server. Chunks may arrive in any order and more than
* once; what is on disk only advances when the gap at baseChunk is filled.
*/
static void CL_ParseDownload( msg_t *msg )
{
	size_t chunk, len, expected;
	int dlId;
	bool outOfOrder = false;
	TracyCZoneN( ctx, "CL_ParseDownload", 1 );

	// read the header. every path below consumes exactly len payload bytes before returning, or
	// the opcode loop in CL_ParseServerMessage would take one of them for an opcode
	dlId = MSG_ReadByte( msg );
	chunk = (size_t)(unsigned)MSG_ReadLong( msg );
	len = (size_t)( MSG_ReadShort( msg ) & 0xffff );

	if( msg->readcount + len > msg->cursize )
	{
		Com_Printf( "Error: Download message didn't have as much data as it promised\n" );
		CL_RetryDownload();
		TracyCZoneEnd( ctx );
		return;
	}

	if( cls.demo.playing )
	{
		// ignore download commands coming from demo files
		msg->readcount += len;
		TracyCZoneEnd( ctx );
		return;
	}

	// a chunk from a download that has already been replaced or finished. this is expected -
	// the server's window may still have been in flight - so it is dropped without counting as
	// a failure, since five of those in a row would abort the transfer
	if( !cls.download.filenum || !cls.download.reorder || dlId != cls.download.dlId )
	{
		msg->readcount += len;
		TracyCZoneEnd( ctx );
		return;
	}

	// a chunk is a full DOWNLOAD_CHUNK_SIZE except the last one, so its length is not something
	// we have to take the server's word for
	expected = 0;
	if( chunk < cls.download.numChunks )
	{
		expected = cls.download.size - chunk * DOWNLOAD_CHUNK_SIZE;
		if( expected > DOWNLOAD_CHUNK_SIZE )
			expected = DOWNLOAD_CHUNK_SIZE;
	}

	if( chunk >= cls.download.numChunks || len != expected )
	{
		Com_Printf( "Error: Invalid download message\n" );
		msg->readcount += len;
		CL_RetryDownload();
		TracyCZoneEnd( ctx );
		return;
	}

	if( chunk < cls.download.baseChunk )
	{
		// already on disk, so the server is still missing the ack that said so - either it was
		// lost or it crossed this in flight. say it again rather than sitting quiet until the
		// retry timer, which is what would otherwise stall the tail of a transfer for 3 seconds
		msg->readcount += len;
		cls.download.timeout = Sys_Milliseconds() + 3000;
		if( !cls.download.ackFlushTime )
			cls.download.ackFlushTime = Sys_Milliseconds() + 1;
		TracyCZoneEnd( ctx );
		return;
	}

	if( chunk - cls.download.baseChunk >= DOWNLOAD_ACK_BITS )
	{
		// past what the ring can hold and past what we could ever report, so the server has
		// overrun its window. drop it rather than aliasing another chunk's slot
		Com_Printf( "Error: Download chunk outside the window\n" );
		msg->readcount += len;
		CL_RetryDownload();
		TracyCZoneEnd( ctx );
		return;
	}

	if( chunk == cls.download.baseChunk )
	{
		cls.download.bytesReceived += len - cls.download.resumeSkip;
		CL_DownloadFlushChunk( msg->data + msg->readcount, len );
		CL_DownloadDrainReorder();
	}
	else if( !DL_BitGet( cls.download.bits, chunk & DOWNLOAD_ACK_MASK ) )
	{
		memcpy( cls.download.reorder + ( chunk & DOWNLOAD_ACK_MASK ) * DOWNLOAD_CHUNK_SIZE,
			msg->data + msg->readcount, len );
		DL_BitSet( cls.download.bits, chunk & DOWNLOAD_ACK_MASK );
		cls.download.bytesReceived += len;
		outOfOrder = true;
	}
	else
	{
		// a duplicate of something already buffered, so likewise: the bit for it is already set
		// and the server has not heard that
		msg->readcount += len;
		cls.download.timeout = Sys_Milliseconds() + 3000;
		if( !cls.download.ackFlushTime )
			cls.download.ackFlushTime = Sys_Milliseconds() + 1;
		TracyCZoneEnd( ctx );
		return;
	}

	msg->readcount += len;

	cls.download.percent = cls.download.size
		? (double)cls.download.bytesReceived / (double)cls.download.size : 1.0;
	clamp( cls.download.percent, 0, 1 );
	Cvar_ForceSet( "cl_download_percent", va( "%.1f", cls.download.percent * 100 ) );

	if( cls.download.baseChunk >= cls.download.numChunks )
	{
		Com_Printf( "Download complete: %s\n", cls.download.name );

		CL_DownloadComplete();

		// let the server know we're done
		CL_AddReliableCommand( va( "nextdl \"%s\" %i", cls.download.origname, -1 ) );

		CL_DownloadDone();
		TracyCZoneEnd( ctx );
		return;
	}

	cls.download.timeout = Sys_Milliseconds() + 3000;
	cls.download.retries = 0;

	// the ack is only ever armed here, never sent - CL_CheckDownloadTimeout sends it at the end
	// of the frame. one message carries up to DOWNLOAD_MAX_RUN_CHUNKS chunks and this runs once
	// per chunk, so sending from here would put a packet on the wire for every chunk in it.
	//
	// due immediately once the base has moved a whole interval, and on a chunk that arrived out
	// of order - that one means something was lost, and the sooner the server is told which, the
	// sooner it comes back. otherwise a lazy timer, so the tail of the file and any hole the
	// server has already filled around still get reported without waiting on the 3 second retry
	if( cls.download.baseChunk - cls.download.lastAckChunk >= cls.download.ackChunks || outOfOrder )
		cls.download.ackFlushTime = Sys_Milliseconds() + 1;
	else if( !cls.download.ackFlushTime )
		cls.download.ackFlushTime = Sys_Milliseconds() + 250;
}

/*
=====================================================================

SERVER CONNECTING MESSAGES

=====================================================================
*/

/*
* CL_ParseServerData
*/
static void CL_ParseServerData( msg_t *msg )
{
	const char *str, *gamedir, *serverinfo;
	int i, sv_bitflags, numpure;
	int http_portnum;
	bool old_sv_pure;
	TracyCZoneN( ctx, "CL_ParseServerData", 1 );

	Com_DPrintf( "Serverdata packet received.\n" );

	// wipe the client_state_t struct

	CL_ClearState();
	CL_SetClientState( CA_CONNECTED );

	// parse protocol version number
	i = MSG_ReadLong( msg );

	if( i != APP_PROTOCOL_VERSION && !(cls.demo.playing && i == APP_DEMO_PROTOCOL_VERSION) )
		Com_Error( ERR_DROP, "Server returned version %i, not %i", i, APP_PROTOCOL_VERSION );

	cl.servercount = MSG_ReadLong( msg );
	cl.snapFrameTime = (unsigned int)MSG_ReadShort( msg );
	cl.gamestart = true;

	// set extrapolation time to half snapshot time
	Cvar_ForceSet( "cl_extrapolationTime", va( "%i", (unsigned int)( cl.snapFrameTime * 0.5 ) ) );
	cl_extrapolationTime->modified = false;

	// base game directory
	str = MSG_ReadString( msg );
	if( !str || !str[0] )
		Com_Error( ERR_DROP, "Server sent an empty base game directory" );
	if( !COM_ValidateRelativeFilename( str ) || strchr( str, '/' ) )
		Com_Error( ERR_DROP, "Server sent an invalid base game directory: %s", str );
	if( strcmp( FS_BaseGameDirectory(), str ) )
	{
		Com_Error( ERR_DROP, "Server has different base game directory (%s) than the client (%s)", str,
			FS_BaseGameDirectory() );
	}

	// game directory
	str = MSG_ReadString( msg );
	if( !str || !str[0] )
		Com_Error( ERR_DROP, "Server sent an empty game directory" );
	if( !COM_ValidateRelativeFilename( str ) || strchr( str, '/' ) )
		Com_Error( ERR_DROP, "Server sent an invalid game directory: %s", str );
	gamedir = FS_GameDirectory();
	if( strcmp( str, gamedir ) )
	{
		// shutdown the cgame module first in case it is running for whatever reason
		// (happens on wswtv in lobby), otherwise precaches that are going to follow
		// will probably fuck up (like models trying to load before the world model)
		CL_GameModule_Shutdown();

		if( !FS_SetGameDirectory( str, true ) )
			Com_Error( ERR_DROP, "Failed to load game directory set by server: %s", str );
		ML_Restart( true );
	}

	// parse player entity number
	cl.playernum = MSG_ReadShort( msg );

	// get the full level name
	Q_strncpyz( cl.servermessage, MSG_ReadString( msg ), sizeof( cl.servermessage ) );

	sv_bitflags = MSG_ReadByte( msg );

	if( cls.demo.playing )
	{
		cls.reliable = ( sv_bitflags & SV_BITFLAGS_RELIABLE );
	}
	else
	{
		if( cls.reliable != ( ( sv_bitflags & SV_BITFLAGS_RELIABLE ) != 0 ) )
			Com_Error( ERR_DROP, "Server and client disagree about connection reliability" );
	}

	// builting HTTP server port
	if( cls.httpbaseurl ) {
		Mem_Free( cls.httpbaseurl );
		cls.httpbaseurl = NULL;
	}

	if( ( sv_bitflags & SV_BITFLAGS_HTTP ) != 0 ) {
		if( ( sv_bitflags & SV_BITFLAGS_HTTP_BASEURL ) != 0 ) {
			// read base upstream url
			cls.httpbaseurl = ZoneCopyString( MSG_ReadString( msg ) );
		}
		else {
			http_portnum = MSG_ReadShort( msg ) & 0xffff;
			cls.httpaddress = cls.serveraddress;
			NET_SetAddressPort( &cls.httpaddress, http_portnum );
			if( http_portnum ) {
				switch( cls.httpaddress.type ) {
					case NA_LOOPBACK:
						cls.httpbaseurl = ZoneCopyString( va( "http://localhost:%hu/", http_portnum ) );
						break;
					case NA_IP:
					case NA_IP6:
						cls.httpbaseurl = ZoneCopyString( va( "http://%s/", NET_AddressToString( &cls.httpaddress ) ) );
						break;
					default:
						// the server's builtin HTTP server only listens on IP sockets, and a relayed
						// address has no reachable host:port form, so downloads go over the game socket
						Com_DPrintf( "Server offers HTTP downloads on port %hu, but %s is not an IP address."
							" Downloading over the game connection instead.\n",
							http_portnum, NET_AddressToString( &cls.httpaddress ) );
						break;
				}
			}
		}
	}

	// pure list

	// clean old, if necessary
	Com_FreePureList( &cls.purelist );

	// add new
	numpure = MSG_ReadShort( msg );
	while( numpure > 0 )
	{
		const char *pakname = MSG_ReadString( msg );
		const unsigned checksum = MSG_ReadLong( msg );

		Com_AddPakToPureList( &cls.purelist, pakname, checksum, NULL );

		numpure--;
	}

	//assert( numpure == 0 );

	// get the configstrings request
	CL_AddReliableCommand( va( "configstrings %i 0", cl.servercount ) );

	old_sv_pure = cls.sv_pure;
	cls.sv_pure = ( sv_bitflags & SV_BITFLAGS_PURE ) != 0;
	cls.pure_restart = cls.sv_pure && old_sv_pure == false;
	cls.sv_tv = ( sv_bitflags & SV_BITFLAGS_TVSERVER ) != 0;

#ifdef PURE_CHEAT
	cls.sv_pure = cls.pure_restart = false;
#endif

	cls.wakelock = Sys_AcquireWakeLock();

	if( !cls.demo.playing && ( cls.serveraddress.type != NA_LOOPBACK ) )
		Steam_AdvertiseGame( &cls.serveraddress, NULL);

	// separate the printfs so the server message can have a color
	Com_Printf( S_COLOR_WHITE "\n" "=====================================\n" );
	Com_Printf( S_COLOR_WHITE "%s\n\n", cl.servermessage );

	TracyCZoneEnd( ctx );
}

/*
* CL_ParseBaseline
*/
static void CL_ParseBaseline( msg_t *msg )
{
	TracyCZoneN( ctx, "CL_ParseBaseline", 1 );
	SNAP_ParseBaseline( msg, cl_baselines );
	TracyCZoneEnd( ctx );
}

/*
* CL_ParseFrame
*/
static void CL_ParseFrame( msg_t *msg )
{
	snapshot_t *snap, *oldSnap;
	int delta;
	TracyCZoneN( ctx, "CL_ParseFrame", 1 );

	oldSnap = ( cl.receivedSnapNum > 0 ) ? &cl.snapShots[cl.receivedSnapNum & UPDATE_MASK] : NULL;

	snap = SNAP_ParseFrame( msg, oldSnap, &cl.suppressCount, cl.snapShots, cl_baselines, cl_shownet->integer );
	if( snap->valid )
	{
		cl.receivedSnapNum = snap->serverFrame;

		if( cls.demo.recording )
		{
			if( cls.demo.waiting && !snap->delta )
			{
				cls.demo.waiting = false; // we can start recording now
				cls.demo.basetime = snap->serverTime;
				cls.demo.localtime = time( NULL );

				// clear demo meta data, we'll write some keys later
				cls.demo.meta_data_realsize = SNAP_ClearDemoMeta( cls.demo.meta_data, sizeof( cls.demo.meta_data ) );

				// write out messages to hold the startup information
				SNAP_BeginDemoRecording( cls.demo.file, 0x10000 + cl.servercount, cl.snapFrameTime, 
					cl.servermessage, cls.reliable ? SV_BITFLAGS_RELIABLE : 0, cls.purelist, 
					cl.configstrings[0], cl_baselines );

				// the rest of the demo file will be individual frames
			}

			if( !cls.demo.waiting )
				cls.demo.duration = snap->serverTime - cls.demo.basetime;
			cls.demo.time = cls.demo.duration;
		}

		if( cl_debug_timeDelta->integer )
		{
			if( oldSnap != NULL && ( oldSnap->serverFrame + 1 != snap->serverFrame ) )
				Com_Printf( S_COLOR_RED"***** SnapShot lost\n" );
		}

		// the first snap, fill all the timeDeltas with the same value
		// don't let delta add big jumps to the smoothing ( a stable connection produces jumps inside +-3 range)
		delta = ( snap->serverTime - cl.snapFrameTime ) - cls.gametime;
		if( cl.currentSnapNum <= 0 || delta < cl.newServerTimeDelta - 175 || delta > cl.newServerTimeDelta + 175 )
		{
			CL_RestartTimeDeltas( delta );
		}
		else
		{
			if( cl_debug_timeDelta->integer )
			{
				if( delta < cl.newServerTimeDelta - (int)cl.snapFrameTime )
					Com_Printf( S_COLOR_CYAN"***** timeDelta low clamp\n" );
				else if( delta > cl.newServerTimeDelta + (int)cl.snapFrameTime )
					Com_Printf( S_COLOR_CYAN"***** timeDelta high clamp\n" );
			}

			clamp( delta, cl.newServerTimeDelta - (int)cl.snapFrameTime, cl.newServerTimeDelta + (int)cl.snapFrameTime );

			cl.serverTimeDeltas[cl.receivedSnapNum & MASK_TIMEDELTAS_BACKUP] = delta;
		}
	}
	TracyCZoneEnd( ctx );
}

//========= StringCommands================

/*
* CL_Multiview_f
*/
static void CL_Multiview_f( void )
{
	cls.mv = ( atoi( Cmd_Argv( 1 ) ) != 0 );
	Com_Printf( "multiview: %i\n", cls.mv );
}

/*
* CL_CvarInfoRequest_f
*/
static void CL_CvarInfoRequest_f( void )
{
	char string[MAX_STRING_CHARS];
	char *cvarName;
	const char *cvarString;

	if( cls.demo.playing )
		return;

	if( Cmd_Argc() < 1 )
		return;

	cvarName = Cmd_Argv( 1 );

	string[0] = 0;
	Q_strncatz( string, "cvarinfo \"", sizeof( string ) );

	if( strlen( string ) + strlen( cvarName ) + 1 /*quote*/ + 1 /*space*/ >= MAX_STRING_CHARS - 1 )
	{
		CL_AddReliableCommand( "cvarinfo \"invalid\"" );
		return;
	}

	Q_strncatz( string, cvarName, sizeof( string ) );
	Q_strncatz( string, "\" ", sizeof( string ) );

	cvarString = Cvar_String( cvarName );
	if( !cvarString[0] )
		cvarString = "not found";

	if( strlen( string ) + strlen( cvarString ) + 2 /*quotes*/ >= MAX_STRING_CHARS - 1 )
	{
		if( strlen( string ) + strlen( " \"too long\"" ) < MAX_STRING_CHARS - 1 )
			CL_AddReliableCommand( va( "%s\"too long\"", string ) );
		else
			CL_AddReliableCommand( "cvarinfo \"invalid\"" );
			
		return;
	}

	Q_strncatz( string, "\"", sizeof( string ) );
	Q_strncatz( string, cvarString, sizeof( string ) );
	Q_strncatz( string, "\"", sizeof( string ) );

	CL_AddReliableCommand( string );
}

/*
* CL_UpdateConfigString
*/
static void CL_UpdateConfigString( int idx, const char *s )
{
	if( !s )
		return;

	if( cl_debug_serverCmd->integer && ( cls.state >= CA_ACTIVE || cls.demo.playing ) )
		Com_Printf( "CL_ParseConfigstringCommand(%i): \"%s\"\n", idx, s );

	if( idx < 0 || idx >= MAX_CONFIGSTRINGS )
		Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );

	// wsw : jal : warn if configstring overflow
	if( strlen( s ) >= MAX_CONFIGSTRING_CHARS )
	{
		Com_Printf( "%sWARNING:%s Configstring %i overflowed\n", S_COLOR_YELLOW, S_COLOR_WHITE, idx );
		Com_Printf( "%s%s\n", S_COLOR_WHITE, s );
	}

	if( !COM_ValidateConfigstring( s ) )
	{
		Com_Printf( "%sWARNING:%s Invalid Configstring (%i): %s\n", S_COLOR_YELLOW, S_COLOR_WHITE, idx, s );
		return;
	}

	Q_strncpyz( cl.configstrings[idx], s, sizeof( cl.configstrings[idx] ) );

	// allow cgame to update it too
	CL_GameModule_ConfigString( idx, s );
}

/*
* CL_ParseConfigstringCommand
*/
static void CL_ParseConfigstringCommand( void )
{
	int i, argc, idx;
	char *s;
	TracyCZoneN( ctx, "CL_ParseConfigstringCommand", 1 );

	if( Cmd_Argc() < 3 ) {
		TracyCZoneEnd( ctx );
		return;
	}

	// ch : configstrings may come batched now, so lets loop through them
	argc = Cmd_Argc();
	for( i = 1; i < argc - 1; i += 2 )
	{
		idx = atoi( Cmd_Argv( i ) );
		s = Cmd_Argv( i + 1 );


		CL_UpdateConfigString( idx, s );
	}
	TracyCZoneEnd( ctx );
}

static void CL_RPC_cb_steamAuth( void *self, struct steam_rpc_pkt_s *rec ){
	//SteamAuthTicket_t* ticket = (SteamAuthTicket_t*)self;
	//ticket->pcbTicket = rec->auth_session.pcbTicket;
	//ticket->pTicket = rec->auth_session.pcbTicket;

	uint8_t messageData[MAX_MSGLEN];
	msg_t msg;
	MSG_Init( &msg, messageData, sizeof( messageData ) );
	MSG_WriteByte( &msg, clc_steamauth );
	MSG_WriteLong( &msg, rec->auth_session.pcbTicket );
	MSG_WriteData( &msg, rec->auth_session.ticket, rec->auth_session.pcbTicket );
	CL_Netchan_Transmit( &msg, NET_SEND_UNRELIABLE );
}

static void CL_SteamAuth(){
	if (STEAMSHIM_active())
	{
		struct steam_rpc_shim_common_s request;
		request.cmd = RPC_AUTHSESSION_TICKET;
		uint32_t syncIndex;
		STEAMSHIM_sendRPC( &request, sizeof( struct steam_rpc_shim_common_s ), NULL, CL_RPC_cb_steamAuth, &syncIndex );
		STEAMSHIM_waitDispatchSync(syncIndex);
	}
}

static void CL_AjaxRespond_f( void )
{
	char *response = Cmd_Argv(1);
	char *data = Cmd_Argv(2);

	CL_UIModule_AjaxResponse(response, data);
}

typedef struct
{
	char *name;
	void ( *func )( void );
} svcmd_t;

svcmd_t svcmds[] =
{
	{ "forcereconnect", CL_Reconnect_f },
	{ "reconnect", CL_ServerReconnect_f },
	{ "changing", CL_Changing_f },
	{ "precache", CL_Precache_f },
	{ "cmd", CL_ForwardToServer_f },
	{ "cs", CL_ParseConfigstringCommand },
	{ "initdownload", CL_InitDownload_f },
	{ "disc", CL_ServerDisconnect_f },
	{ "disconnect", CL_ServerDisconnect_f },
	{ "multiview", CL_Multiview_f },
	{ "cvarinfo", CL_CvarInfoRequest_f },
	{ "steamauth", CL_SteamAuth },
	{ "ajaxrespond", CL_AjaxRespond_f },

	{ NULL, NULL }
};


/*
* CL_ParseServerCommand
*/
static void CL_ParseServerCommand( msg_t *msg )
{
	const char *s;
	char *text;
	svcmd_t *cmd;
	TracyCZoneN( ctx, "CL_ParseServerCommand", 1 );

	text = MSG_ReadString( msg );

	Cmd_TokenizeString( text );
	s = Cmd_Argv( 0 );

	if( cl_debug_serverCmd->integer && ( cls.state < CA_ACTIVE || cls.demo.playing ) )
		Com_Printf( "CL_ParseServerCommand: \"%s\"\n", text );

	// filter out these server commands to be called from the client
	for( cmd = svcmds; cmd->name; cmd++ )
	{
		if( !strcmp( s, cmd->name ) )
		{
			cmd->func();
			TracyCZoneEnd( ctx );
			return;
		}
	}

	Com_Printf( "Unknown server command: %s\n", s );
	TracyCZoneEnd( ctx );
}

static void CB_RPC_DecompressVoice( void *self, struct steam_rpc_pkt_s *rec )
{
	int clientnum = (int)self;
	CL_GameModule_PlayVoice(rec->decompress_voice_recv.buffer, rec->decompress_voice_recv.count, clientnum);
}

static void CL_ParseVoiceData( msg_t *msg ) {
	int client = MSG_ReadShort( msg );
	TracyCZoneN( ctx, "CL_ParseVoiceData", 1 );

	int size = MSG_ReadShort( msg );
	if (cl_enablevoice->integer != 1) {
		MSG_SkipData(msg, size);
		TracyCZoneEnd( ctx );
		return;
	}

	if (size > VOICE_BUFFER_MAX) {
		TracyCZoneEnd( ctx );
		return;
	}

	struct decompress_voice_req_s *req = (struct decompress_voice_req_s *)malloc(sizeof(struct decompress_voice_req_s) + size);

	req->cmd = RPC_DECOMPRESS_VOICE;
	req->count = size;
	MSG_ReadData( msg, req->buffer, size );

	// yes this is bad but i'm not making an allocation for a single int
	STEAMSHIM_sendRPC(req, sizeof(struct decompress_voice_req_s) + size, (int*)client, CB_RPC_DecompressVoice, NULL);
	TracyCZoneEnd( ctx );
}

/*
=====================================================================

ACTION MESSAGES

=====================================================================
*/

/*
* CL_ParseServerMessage
*/
void CL_ParseServerMessage( msg_t *msg )
{
	int cmd;
	TracyCZoneN( ctx, "CL_ParseServerMessage", 1 );

	if( cl_shownet->integer == 1 )
	{
		Com_Printf( "%i ", msg->cursize );
	}
	else if( cl_shownet->integer >= 2 )
	{
		Com_Printf( "------------------\n" );
	}

	// parse the message
	while( 1 )
	{
		if( msg->readcount > msg->cursize )
		{
			Com_Error( ERR_DROP, "CL_ParseServerMessage: Bad server message" );
			break;
		}

		cmd = MSG_ReadByte( msg );
		if( cl_debug_serverCmd->integer & 4 )
		{
			if( cmd == -1 )
				Com_Printf( "%3i:CMD %i %s\n", msg->readcount-1, cmd, "EOF" );
			else
				Com_Printf( "%3i:CMD %i %s\n", msg->readcount-1, cmd, !svc_strings[cmd] ? "bad" : svc_strings[cmd] );
		}

		if( cmd == -1 )
		{
			SHOWNET( msg, "END OF MESSAGE" );
			break;
		}

		if( cl_shownet->integer >= 2 )
		{
			if( !svc_strings[cmd] )
				Com_Printf( "%3i:BAD CMD %i\n", msg->readcount-1, cmd );
			else
				SHOWNET( msg, svc_strings[cmd] );
		}

		// other commands
		switch( cmd )
		{
		default:
			Com_Error( ERR_DROP, "CL_ParseServerMessage: Illegible server message" );
			break;

		case svc_nop:
			// Com_Printf( "svc_nop\n" );
			break;

		case svc_servercmd:
			if( !cls.reliable )
			{
				int cmdNum = MSG_ReadLong( msg );
				if( cmdNum < 0 )
				{
					Com_Error( ERR_DROP, "CL_ParseServerMessage: Invalid cmdNum value received: %i\n",
						cmdNum );
					return;
				}
				if( cmdNum <= cls.lastExecutedServerCommand )
				{
					MSG_ReadString( msg ); // read but ignore
					break;
				}
				cls.lastExecutedServerCommand = cmdNum;
			}
			// fall through
		case svc_servercs: // configstrings from demo files. they don't have acknowledge
			CL_ParseServerCommand( msg );	
			break;

		case svc_serverdata:
			if( cls.state == CA_HANDSHAKE )
			{
				Cbuf_Execute(); // make sure any stuffed commands are done
				CL_ParseServerData( msg );
			}
			else
			{
				TracyCZoneEnd( ctx );
				return; // ignore rest of the packet (serverdata is always sent alone)
			}
			break;

		case svc_spawnbaseline:
			CL_ParseBaseline( msg );
			break;

		case svc_download:
			CL_ParseDownload( msg );
			break;

		case svc_clcack:
			if( cls.reliable )
			{
				Com_Error( ERR_DROP, "CL_ParseServerMessage: clack message for reliable client\n" );
				return;
			}
			cls.reliableAcknowledge = (unsigned)MSG_ReadLong( msg );
			cls.ucmdAcknowledged = (unsigned)MSG_ReadLong( msg );
			if( cl_debug_serverCmd->integer & 4 )
				Com_Printf( "svc_clcack:reliable cmd ack:%i ucmdack:%i\n", cls.reliableAcknowledge, cls.ucmdAcknowledged );
			break;

		case svc_frame:
			CL_ParseFrame( msg );
			break;

		case svc_demoinfo:
			assert( cls.demo.playing );
			{
				size_t meta_data_maxsize;

				MSG_ReadLong( msg );
				MSG_ReadLong( msg );
				cls.demo.meta_data_realsize = (size_t)MSG_ReadLong( msg );
				meta_data_maxsize = (size_t)MSG_ReadLong( msg );

				// sanity check
				if( cls.demo.meta_data_realsize > meta_data_maxsize ) {
					cls.demo.meta_data_realsize = meta_data_maxsize;
				}
				if( cls.demo.meta_data_realsize > sizeof( cls.demo.meta_data ) ) {
					cls.demo.meta_data_realsize = sizeof( cls.demo.meta_data );
				}

				MSG_ReadData( msg, cls.demo.meta_data, cls.demo.meta_data_realsize );
				MSG_SkipData( msg, meta_data_maxsize - cls.demo.meta_data_realsize );
			}
			break;

		case svc_playerinfo:
		case svc_packetentities:
		case svc_match:
			Com_Error( ERR_DROP, "Out of place frame data" );
			break;

		case svc_extension:
			if( 1 )
			{
				int ext, len;

				ext = MSG_ReadByte( msg );		// extension id
				MSG_ReadByte( msg );			// version number
				len = MSG_ReadShort( msg );		// command length

				switch( ext )
				{
				default:
					// unsupported
					MSG_SkipData( msg, len );
					break;
				}
			}
			break;
		case svc_voice:
			{
				CL_ParseVoiceData( msg );
				break;
			}
		}
	}

	CL_AddNetgraph();

	//
	// if recording demos, copy the message out
	//
	//
	// we don't know if it is ok to save a demo message until
	// after we have parsed the frame
	//
	if( cls.demo.recording && !cls.demo.waiting )
		CL_WriteDemoMessage( msg );

	TracyCZoneEnd( ctx );
}
