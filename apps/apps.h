/* NodyOS - built-in applications. */
#ifndef NODY_APPS_H
#define NODY_APPS_H

#include "gui/wm.h"

/* Flow: node-based 3D VFX (flow/flow.c) */
void    flow_launch(void);
Window *flow_window(void);
void    flow_open_file(const char *name);
void    flow_install_examples(void);    /* writes demo scenes to a fresh disk */

/* Files: NodyFS browser */
void    files_launch(void);
Window *files_window(void);

/* System: machine information, restart, shut down */
void    sysinfo_launch(void);
Window *sysinfo_window(void);

void    apps_register(void);            /* adds all of the above to the dock */

#endif
