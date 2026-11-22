/**
 * @file slsettingsbackup.h
 * @brief Preferences > Advanced: Back up settings, Restore settings, Export for another
 *        viewer (#1423). The file rules live in slsettingsbackupcore (tested on its own).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_SETTINGSBACKUP_H
#define SL_SETTINGSBACKUP_H

namespace SLSettingsBackup
{
// Ask whether to include chat history, pick a file, write the backup.
void backUp();
// Pick a backup, show what it restores, then stage it for the next start.
void restore();
// Write the settings another viewer shares (Firestorm / Second Life) into its settings,
// with a backup copy, or to a file.
void exportForViewer();
// Startup, before the user settings files are read: apply a staged restore.
void applyPendingRestore();
}

#endif
