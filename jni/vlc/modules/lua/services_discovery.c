/*****************************************************************************
 * services_discovery.c : Services discovery using lua scripts
 *****************************************************************************
 * Copyright (C) 2010 VideoLAN and AUTHORS
 *
 * Authors: Fabio Ritrovato <sephiroth87 at videolan dot org>
 *          Rémi Duraffort <ivoire at videolan -dot- org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_services_discovery.h>

#include "vlc.h"
#include "libs.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void *Run( void * );
static int DoSearch( services_discovery_t *p_sd, const char *psz_query );
static int FillDescriptor( services_discovery_t *, services_discovery_descriptor_t * );
static int Control( services_discovery_t *p_sd, int i_command, va_list args );

static const char * const ppsz_sd_options[] = { "sd", "longname", NULL };

/*****************************************************************************
 * Local structures
 *****************************************************************************/
struct services_discovery_sys_t
{
    lua_State *L;
    char *psz_filename;

    vlc_thread_t thread;
    vlc_mutex_t lock;
    vlc_cond_t cond;
    bool b_exiting;

    char **ppsz_query;
    int i_query;
};
static const luaL_Reg p_reg[] = { { NULL, NULL } };

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
int Open_LuaSD( vlc_object_t *p_this )
{
    services_discovery_t *p_sd = ( services_discovery_t * )p_this;
    services_discovery_sys_t *p_sys;
    lua_State *L = NULL;
    char *psz_name;

    if( !strcmp( p_sd->psz_name, "lua" ) )
    {
        // We want to load the module name "lua"
        // This module can be used to load lua script not registered
        // as builtin lua SD modules.
        config_ChainParse( p_sd, "lua-", ppsz_sd_options, p_sd->p_cfg );
        psz_name = var_GetString( p_sd, "lua-sd" );
    }
    else
    {
        // We are loading a builtin lua sd module.
        psz_name = strdup(p_sd->psz_name);
    }

    if( !( p_sys = malloc( sizeof( services_discovery_sys_t ) ) ) )
    {
        free( psz_name );
        return VLC_ENOMEM;
    }
    p_sd->p_sys = p_sys;
    p_sd->pf_control = Control;
    p_sys->psz_filename = vlclua_find_file( p_this, "sd", psz_name );
    if( !p_sys->psz_filename )
    {
        msg_Err( p_sd, "Couldn't find lua services discovery script \"%s\".",
                 psz_name );
        free( psz_name );
        goto error;
    }
    free( psz_name );
    L = luaL_newstate();
    if( !L )
    {
        msg_Err( p_sd, "Could not create new Lua State" );
        goto error;
    }
    vlclua_set_this( L, p_sd );
    luaL_openlibs( L );
    luaL_register( L, "vlc", p_reg );
    luaopen_input( L );
    luaopen_msg( L );
    luaopen_misc( L );
    luaopen_net( L );
    luaopen_object( L );
    luaopen_sd( L );
    luaopen_strings( L );
    luaopen_variables( L );
    luaopen_stream( L );
    luaopen_gettext( L );
    luaopen_xml( L );
    luaopen_md5( L );
    lua_pop( L, 1 );

    if( vlclua_add_modules_path( p_sd, L, p_sys->psz_filename ) )
    {
        msg_Warn( p_sd, "Error while setting the module search path for %s",
                  p_sys->psz_filename );
        goto error;
    }
    if( luaL_dofile( L, p_sys->psz_filename ) )
    {
        msg_Err( p_sd, "Error loading script %s: %s", p_sys->psz_filename,
                  lua_tostring( L, lua_gettop( L ) ) );
        lua_pop( L, 1 );
        goto error;
    }
    p_sys->L = L;
    vlc_mutex_init( &p_sys->lock );
    vlc_cond_init( &p_sys->cond );
    p_sys->b_exiting = false;
    TAB_INIT( p_sys->i_query, p_sys->ppsz_query );

    if( vlc_clone( &p_sys->thread, Run, p_sd, VLC_THREAD_PRIORITY_LOW ) )
    {
        TAB_CLEAN( p_sys->i_query, p_sys->ppsz_query );
        vlc_cond_destroy( &p_sys->cond );
        vlc_mutex_destroy( &p_sys->lock );
        goto error;
    }
    return VLC_SUCCESS;

error:
    if( L )
        lua_close( L );
    free( p_sys->psz_filename );
    free( p_sys );
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: cleanup
 *****************************************************************************/
void Close_LuaSD( vlc_object_t *p_this )
{
    services_discovery_t *p_sd = ( services_discovery_t * )p_this;
    services_discovery_sys_t *p_sys = p_sd->p_sys;

    vlc_mutex_lock( &p_sys->lock );
    p_sys->b_exiting = true;
    vlc_mutex_unlock( &p_sys->lock );

    vlc_cancel( p_sys->thread );
    vlc_join( p_sys->thread, NULL );

    for( int i = 0; i < p_sys->i_query; i++ )
        free( p_sys->ppsz_query[i] );
    TAB_CLEAN( p_sys->i_query, p_sys->ppsz_query );

    vlc_cond_destroy( &p_sys->cond );
    vlc_mutex_destroy( &p_sys->lock );
    free( p_sys->psz_filename );
    lua_close( p_sys->L );
    free( p_sys );
}

/*****************************************************************************
 * Run: Thread entry-point
 ****************************************************************************/
static void* Run( void *data )
{
    services_discovery_t *p_sd = ( services_discovery_t * )data;
    services_discovery_sys_t *p_sys = p_sd->p_sys;
    lua_State *L = p_sys->L;

    int cancel = vlc_savecancel();

    lua_getglobal( L, "main" );
    if( !lua_isfunction( L, lua_gettop( L ) ) || lua_pcall( L, 0, 1, 0 ) )
    {
        msg_Err( p_sd, "Error while running script %s, "
                  "function main(): %s", p_sys->psz_filename,
                  lua_tostring( L, lua_gettop( L ) ) );
        lua_pop( L, 1 );
        vlc_restorecancel( cancel );
        return NULL;
    }
    msg_Dbg( p_sd, "LuaSD script loaded: %s", p_sys->psz_filename );

    /* Force garbage collection, because the core will keep the SD
     * open, but lua will never gc until lua_close(). */
    lua_gc( L, LUA_GCCOLLECT, 0 );

    vlc_restorecancel( cancel );

    /* Main loop to handle search requests */
    vlc_mutex_lock( &p_sys->lock );
    mutex_cleanup_push( &p_sys->lock );
    while( !p_sys->b_exiting )
    {
        /* Wait for a request */
        while( !p_sys->i_query )
            vlc_cond_wait( &p_sys->cond, &p_sys->lock );

        /* Execute every query each one protected against cancelation */
        cancel = vlc_savecancel();
        while( !p_sys->b_exiting && p_sys->i_query )
        {
            char *psz_query = p_sys->ppsz_query[p_sys->i_query - 1];
            REMOVE_ELEM( p_sys->ppsz_query, p_sys->i_query, p_sys->i_query - 1 );

            vlc_mutex_unlock( &p_sys->lock );
            DoSearch( p_sd, psz_query );
            free( psz_query );
            vlc_mutex_lock( &p_sys->lock );
        }
        /* Force garbage collection, because the core will keep the SD
         * open, but lua will never gc until lua_close(). */
        lua_gc( L, LUA_GCCOLLECT, 0 );

        vlc_restorecancel( cancel );
    }
    vlc_cleanup_run();

    return NULL;
}

/*****************************************************************************
 * Control: services discrovery control
 ****************************************************************************/
static int Control( services_discovery_t *p_sd, int i_command, va_list args )
{
    services_discovery_sys_t *p_sys = p_sd->p_sys;

    switch( i_command )
    {
    case SD_CMD_SEARCH:
    {
        const char *psz_query = va_arg( args, const char * );
        vlc_mutex_lock( &p_sys->lock );
        TAB_APPEND( p_sys->i_query, p_sys->ppsz_query, strdup( psz_query ) );
        vlc_cond_signal( &p_sys->cond );
        vlc_mutex_unlock( &p_sys->lock );
        break;
    }

    case SD_CMD_DESCRIPTOR:
    {
        services_discovery_descriptor_t *p_desc = va_arg( args,
                                services_discovery_descriptor_t * );
        return FillDescriptor( p_sd, p_desc );
    }
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * DoSearch: search for a given query
 ****************************************************************************/
static int DoSearch( services_discovery_t *p_sd, const char *psz_query )
{
    services_discovery_sys_t *p_sys = p_sd->p_sys;
    lua_State *L = p_sys->L;

    /* Lookup for the 'search' function */
    lua_getglobal( L, "search" );
    if( !lua_isfunction( L, lua_gettop( L ) ) )
    {
        msg_Err( p_sd, "The script '%s' does not define any 'search' function",
                 p_sys->psz_filename );
        lua_pop( L, 1 );
        return VLC_EGENERIC;
    }

    /* Push the query */
    lua_pushstring( L, psz_query );

    /* Call the 'search' function */
    if( lua_pcall( L, 1, 0, 0 ) )
    {
        msg_Err( p_sd, "Error while running the script '%s': %s",
                 p_sys->psz_filename, lua_tostring( L, lua_gettop( L ) ) );
        lua_pop( L, 1 );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/** List of capabilities */
static const char *const ppsz_capabilities[] = {
    "search",
    NULL
};

/*****************************************************************************
 * FillDescriptor: call the descriptor function and fill the structure
 ****************************************************************************/
static int FillDescriptor( services_discovery_t *p_sd,
                           services_discovery_descriptor_t *p_desc )
{
    services_discovery_sys_t *p_sys = p_sd->p_sys;
    int i_ret = VLC_EGENERIC;

    /* Create a new lua thread */
    lua_State *L = luaL_newstate();
    if( luaL_dofile( L, p_sys->psz_filename ) )
    {
        msg_Err( p_sd, "Error loading script %s: %s", p_sys->psz_filename,
                 lua_tostring( L, -1 ) );
        goto end;
    }

    /* Call the "descriptor" function */
    lua_getglobal( L, "descriptor" );
    if( !lua_isfunction( L, -1 ) || lua_pcall( L, 0, 1, 0 ) )
    {
        msg_Warn( p_sd, "Error getting the descriptor in '%s': %s",
                  p_sys->psz_filename, lua_tostring( L, -1 ) );
        goto end;
    }

    /* Get the different fields of the returned table */
    lua_getfield( L, -1, "short_description" );
    p_desc->psz_short_desc = luaL_strdupornull( L, -1 );
    lua_pop( L, 1 );

    lua_getfield( L, -1, "icon" );
    p_desc->psz_icon_url = luaL_strdupornull( L, -1 );
    lua_pop( L, 1 );

    lua_getfield( L, -1, "url" );
    p_desc->psz_url = luaL_strdupornull( L, -1 );
    lua_pop( L, 1 );

    lua_getfield( L, -1, "capabilities" );
    p_desc->i_capabilities = 0;
    if( lua_istable( L, -1 ) )
    {
        /* List all table entries */
        lua_pushnil( L );
        while( lua_next( L, -2 ) != 0 )
        {
            /* Key is at index -2 and value at index -1 */
            const char *psz_cap = luaL_checkstring( L, -1 );
            int i_cap = 0;
            const char *psz_iter;
            for( psz_iter = *ppsz_capabilities; psz_iter;
                 psz_iter = ppsz_capabilities[ ++i_cap ] )
            {
                if( !strcmp( psz_iter, psz_cap ) )
                {
                    p_desc->i_capabilities |= 1 << i_cap;
                    break;
                }
            }
            if( !psz_iter )
                msg_Warn( p_sd, "Services discovery capability '%s' unknown in "
                                "script '%s'", psz_cap, p_sys->psz_filename );
        }
    }
    lua_pop( L, 1 );
    i_ret = VLC_SUCCESS;

end:
    lua_close( L );
    return i_ret;

}
