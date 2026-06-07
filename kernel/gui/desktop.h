#ifndef KERNEL_GUI_DESKTOP_H
#define KERNEL_GUI_DESKTOP_H

/* Initialise the desktop (draws background, creates system windows). */
void desktop_init(void);

/* Update the desktop: redraw clock, process keyboard, refresh dirty areas.
   Called from the GUI kernel thread on each tick. */
void desktop_tick(void);

#endif
