/* GStreamer Wayland video sink
 *
 * Copyright 2018 NXP
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>

#include "wlutils.h"

#define WESTON_INI "/etc/xdg/weston/weston.ini"

gboolean
gst_wl_init_buffer_scale(gint display_width, gint display_height, guint * scale)
{
    gchar path[] = WESTON_INI;
    gchar line[512], *p, *section = NULL, *size = NULL;
    gint fd, n, i;
    gint desktop_width, desktop_height;
    gboolean found_config = FALSE;
    gboolean ret = TRUE;
    struct stat filestat;
    FILE *fp;

    if ((fd = open (path, O_RDONLY)) == -1) {
        return FALSE;
    }

    if (fstat(fd, &filestat) < 0 ||
	    !S_ISREG(filestat.st_mode)) {
        close(fd);
        return FALSE;
    }

    fp = fdopen(fd, "r");
    if (fp == NULL) {
        close(fd);
        return FALSE;
    }

    while (fgets(line, sizeof line, fp)) {
        if (found_config)
            break;

        switch (line[0]) {
            case '#':
            case '\n':
                continue;
            case '[':
                p = strchr(&line[1], ']');
                if (!p || p[1] != '\n') {
                    continue;
                }
                p[0] = '\0';
                if (section)
                    g_free (section);
                section = g_strdup(&line[1]);
                continue;
            default:
                if (section && strcmp (section, "shell") == 0) {
                    p = strchr(line, '=');
                    if (!p || p == line) {
                        continue;
                    }

                    p[0] = '\0';
                    if (strcmp (&line[0], "size") == 0) {
                        p++;
                        while (isspace(*p))
                            p++;
                        i = strlen(p);
                        while (i > 0 && isspace(p[i - 1])) {
                            p[i - 1] = '\0';
                            i--;
                        }
                        if (strlen(p) > 0) {
                            if (size)
                                g_free (size);
                            size = g_strdup(p);
                            found_config = TRUE;
                        }
                    }
                }
                continue;
        }
    }

    if (found_config && size) {
        n = sscanf(size, "%dx%d\n", &desktop_width, &desktop_height);
        if (n != 2) {
            ret = FALSE;
            goto out;
        }
    } else {
        ret = FALSE;
        goto out;
    }

    /* FIXME: only support buffer scale 2 and 1 */
    if (display_width > 0 && display_height > 0) {
        *scale = display_width / desktop_width;
        if (*scale != 1 && *scale != 2) {
            *scale = 1;
        }
    } else {
        ret = FALSE;
        goto out;
    }

out:
    if (section)
        g_free (section);
    if (size)
        g_free (size);
    fclose(fp);
    return ret;
}