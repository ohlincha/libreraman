#pragma once
// LibreRaman was called LibreRamen up to 0.3.9. These carry a user's things over from the old name, once:
// nothing is deleted, so going back to an old version still works.
//   env():        LIBRERAMEN_API_FILE/_URL/_TOKEN are used when the LIBRERAMAN_ ones are not set
//   configDir():  ~/.config/libreramen (calibrations, shared with the Python version) copied to .../libreraman
//   appData():    (application name set) settings and the application data folder (spectrum library) copied
//                 from the LibreRamen ones, if LibreRaman has none yet
namespace lr::legacy {

void env();
void configDir();
void appData();

}  // namespace lr::legacy
