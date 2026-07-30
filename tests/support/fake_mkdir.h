/*
Recording fake for Sys_Mkdir.

FS_CreatePath's whole observable behaviour is which directories it asks the OS to
create, so a stub that swallowed the call would leave nothing to assert on. This
records the paths instead, and creates nothing.
*/

#ifndef FAKE_MKDIR_H
#define FAKE_MKDIR_H

#define FAKE_MKDIR_MAX 32

void fake_mkdir_reset( void );
int fake_mkdir_count( void );
/* The path passed to the index'th Sys_Mkdir call, or "" if out of range. */
const char *fake_mkdir_path( int index );

#endif
