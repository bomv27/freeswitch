/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 *
 * switch_loadable_module.h -- Loadable Modules
 *
 */
/*! \file switch_loadable_module.h
    \brief Loadable Module Routines

	This module is the gateway between external modules and the core of the application.
	it contains all the access points to the various pluggable interfaces including the codecs 
	and API modules.

*/

#ifndef SWITCH_LOADABLE_MODULE_H
#define SWITCH_LOADABLE_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <switch.h>

/*!
  \defgroup mods Loadable Module Functions
  \ingroup core1
  \{ 
*/

/*! \brief The abstraction of a loadable module */
struct switch_loadable_module_interface {
	/*! the name of the module */
	const char *module_name;
	/*! the table of endpoints the module has implmented */
	const switch_endpoint_interface *endpoint_interface;
	/*! the table of timers the module has implmented */
	const switch_timer_interface *timer_interface;
	/*! the table of dialplans the module has implmented */
	const switch_dialplan_interface *dialplan_interface;
	/*! the table of codecs the module has implmented */
	const switch_codec_interface *codec_interface;
	/*! the table of applications the module has implmented */
	const switch_application_interface *application_interface;
	/*! the table of api functions the module has implmented */
	const switch_api_interface *api_interface;
	/*! the table of file formats the module has implmented */
	const switch_file_interface *file_interface;
	/*! the table of speech interfaces the module has implmented */
	const switch_speech_interface *speech_interface;
	/*! the table of directory interfaces the module has implmented */
	const switch_directory_interface *directory_interface;
};

/*!
  \brief Initilize the module backend and load all the modules
  \return SWITCH_STATUS_SUCCESS when complete
 */
SWITCH_DECLARE(switch_status) switch_loadable_module_init(void);

/*!
  \brief Shutdown the module backend and call the shutdown routine in all loaded modules
 */
SWITCH_DECLARE(void) switch_loadable_module_shutdown(void);

/*!
  \brief Retrieve the endpoint interface by it's registered name
  \param name the name of the endpoint
  \return the desired endpoint interface
 */
SWITCH_DECLARE(switch_endpoint_interface *) switch_loadable_module_get_endpoint_interface(char *name);

/*!
  \brief Retrieve the codec interface by it's registered name
  \param name the name of the codec
  \return the desired codec interface
 */
SWITCH_DECLARE(switch_codec_interface *) switch_loadable_module_get_codec_interface(char *name);

/*!
  \brief Retrieve the dialplan interface by it's registered name
  \param name the name of the dialplan
  \return the desired dialplan interface
 */
SWITCH_DECLARE(switch_dialplan_interface *) switch_loadable_module_get_dialplan_interface(char *name);

/*!
  \brief build a dynamic module object and register it (for use in double embeded modules)
  \param filename the name of the modules source file
  \param switch_module_load the function to call when the module is loaded
  \param switch_module_runtime a function requested to be started in it's own thread once loaded
  \param switch_module_shutdown the function to call when the system is shutdown
  \return the resulting status
  \note only use this function if you are making a module that in turn gateways module loading to another technology
 */
SWITCH_DECLARE(switch_status) switch_loadable_module_build_dynamic(char *filename,
																   switch_module_load_t switch_module_load,
																   switch_module_runtime_t switch_module_runtime,
																   switch_module_shutdown_t switch_module_shutdown);

/*!
  \brief Retrieve the dialplan interface by it's registered name
  \param name the name of the dialplan
  \return the desired dialplan interface
 */
SWITCH_DECLARE(switch_status) switch_loadable_module_load_dynamic(switch_loadable_module *new_module);

/*!
  \brief Retrieve the timer interface by it's registered name
  \param name the name of the timer
  \return the desired timer interface
 */
SWITCH_DECLARE(switch_timer_interface *) switch_loadable_module_get_timer_interface(char *name);

/*!
  \brief Retrieve the application interface by it's registered name
  \param name the name of the application
  \return the desired application interface
 */
SWITCH_DECLARE(switch_application_interface *) switch_loadable_module_get_application_interface(char *name);

/*!
  \brief Retrieve the API interface by it's registered name
  \param name the name of the API
  \return the desired API interface
 */
SWITCH_DECLARE(switch_api_interface *) switch_loadable_module_get_api_interface(char *name);

/*!
  \brief Retrieve the file format interface by it's registered name
  \param name the name of the file format
  \return the desired file format interface
 */
SWITCH_DECLARE(switch_file_interface *) switch_loadable_module_get_file_interface(char *name);

/*!
  \brief Retrieve the speech interface by it's registered name
  \param name the name of the speech interface
  \return the desired speech interface
 */
SWITCH_DECLARE(switch_speech_interface *) switch_loadable_module_get_speech_interface(char *name);

/*!
  \brief Retrieve the directory interface by it's registered name
  \param name the name of the directory interface
  \return the desired directory interface
 */
SWITCH_DECLARE(switch_directory_interface *) switch_loadable_module_get_directory_interface(char *name);


/*!
  \brief Retrieve the list of loaded codecs into an array
  \param pool the memory pool to use for the hash index
  \param array the array to populate
  \param arraylen the max size in elements of the array
  \return the number of elements added to the array
 */
SWITCH_DECLARE(int) switch_loadable_module_get_codecs(switch_memory_pool *pool, 
													  switch_codec_interface **array, 
													  int arraylen);

/*!
  \brief Retrieve the list of loaded codecs into an array based on another array showing the sorted order
  \param array the array to populate
  \param arraylen the max size in elements of the array
  \param prefs the array of preferred codec names
  \param preflen the size in elements of the prefs
  \return the number of elements added to the array
  \note this function only considers codecs that are listed in the "prefs" array and ignores the rest.
*/
SWITCH_DECLARE(int) switch_loadable_module_get_codecs_sorted(switch_codec_interface **array,
															 int arraylen, 
															 char **prefs, 
															 int preflen);

/*!
  \brief Execute a registered API command
  \param cmd the name of the API command to execute
  \param arg the optional arguement to the command
  \param retbuf a buffer to write output to
  \param len the length in bytes of retbuf
  \return the status returned by the API call
*/
SWITCH_DECLARE(switch_status) switch_api_execute(char *cmd, char *arg, char *retbuf, switch_size_t len);


/*!
  \brief Load a module
  \param dir the directory where the module resides
  \param fname the file name of the module
  \return the status
*/
SWITCH_DECLARE(switch_status) switch_loadable_module_load_module(char *dir, char *fname);

/* Prototypes of module interface functions */

/*!
  \brief Load a module
  \param interface a pointer to a pointer to aim at your module's local interface
  \param filename the path to the module's dll or so file
  \return SWITCH_STATUS_SUCCESS on a successful load
*/
SWITCH_MOD_DECLARE(switch_status) switch_module_load(const switch_loadable_module_interface **interface, char *filename);
SWITCH_MOD_DECLARE(switch_status) switch_module_reload(void);
SWITCH_MOD_DECLARE(switch_status) switch_module_pause(void);
SWITCH_MOD_DECLARE(switch_status) switch_module_resume(void);
SWITCH_MOD_DECLARE(switch_status) switch_module_status(void);
SWITCH_MOD_DECLARE(switch_status) switch_module_runtime(void);


/*!
  \brief Shutdown a module
  \return SWITCH_STATUS_SUCCESS on a successful shutdown
*/
SWITCH_MOD_DECLARE(switch_status) switch_module_shutdown(void);
///\}

#ifdef __cplusplus
}
#endif

#endif
